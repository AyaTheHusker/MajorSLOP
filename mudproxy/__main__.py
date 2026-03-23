import asyncio
import logging
import re
import sys
import threading
import time
from pathlib import Path
from typing import Optional

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
from gi.repository import GLib

from .config import Config
from .proxy import MudProxy
from .parser import MudParser, RoomData, InventoryData
from .gui import MudProxyGUI
from .command_api import CommandAPI
from .autograind import AutoGrind
from .room_renderer import RoomRenderer
from .room_window import RoomWindow
from .character_window import CharacterWindow
from .entity_db import EntityDB
from .ansi import strip_ansi
from .depth_processor import DepthProcessor
from .vulkan_renderer import VulkanDepthRenderer, DepthRenderParams

logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s [%(name)s] %(levelname)s: %(message)s',
    filename='/tmp/mudproxy_debug.log',
    filemode='w',
)
# Suppress noisy httpx/httpcore debug logs
logging.getLogger('httpx').setLevel(logging.WARNING)
logging.getLogger('httpcore').setLevel(logging.WARNING)
logger = logging.getLogger('mudproxy')


class MudProxyApp:
    def __init__(self):
        self.config = Config.load()
        self.proxy = MudProxy(self.config)
        self.parser = MudParser()
        self.parser._char_name_lower = (self.config.character_name or "").split()[0].lower()
        self.gui = MudProxyGUI()
        self._loop: asyncio.AbstractEventLoop = None
        self._hp = 0
        self._mana = 0
        self._current_room = ""
        self._cmd_api = None
        self._autogrind = AutoGrind(inject_fn=self.proxy.inject_command)

        # Room renderer + window (window created on activate)
        self._room_window = None
        self._room_renderer = RoomRenderer(
            model_type="flux-klein-4b",
            width=1024,
            height=768,
            steps=4,
            on_image_ready=self._on_room_image_ready,
            on_thumbnail_ready=self._on_thumbnail_ready,
            get_entity_description=self._get_entity_description,
        )
        self._room_renderer.depth_3d_enabled = self.config.depth_3d_enabled
        self._room_renderer.on_progress = self._on_gen_progress
        self._room_renderer.on_thumb_progress = self._on_thumb_progress
        self._room_renderer.blocked = False  # chatbot off by default, FLUX can load

        # Entity database for creature/NPC/player thumbnails
        # (renderer is created above, so we can set get_entity_description after)
        self._entity_db = EntityDB(
            generate_thumbnail_fn=self._room_renderer.generate_thumbnail,
            on_look_complete=self._on_look_complete,
        )
        self._entity_db.on_status = self._on_entity_status
        self._entity_db.portrait_style = self.config.portrait_style
        self._entity_db.my_char_name = self.config.character_name or ""
        self._auto_look_queue: list[str] = []
        self._auto_looking = False
        self._look_fail_count: dict[str, int] = {}  # name -> consecutive failures
        self._look_max_retries = 3  # give up after this many failed looks
        self._room_look_pending: set[str] = set()  # rooms we've sent 'l' for

        # Character window
        self._char_window = None
        self._char_name = self.config.character_name
        self._char_full_name = ""
        self._who_timer = None
        self._inventory_timer = None

        # Depth 3D system
        self._depth_params = DepthRenderParams.from_dict(self.config.depth_3d_params)
        self._vulkan_renderer: Optional[VulkanDepthRenderer] = None
        self._depth_processor = DepthProcessor(
            python_path=str(Path(__file__).parent.parent / "python" / "bin" / "python3.12"),
            on_depth_ready=self._on_depth_ready,
            inpaint_enabled=self.config.depth_inpaint_enabled,
            is_idle_cb=self._is_renderer_idle,
            offload_gpu_cb=lambda: self._room_renderer.offload_to_cpu(),
            reload_gpu_cb=lambda: self._room_renderer.reload_to_gpu(),
        )
        self._current_room_cache_key: Optional[str] = None
        self._loaded_depth_key: Optional[str] = None  # what's currently on the GPU
        self._last_inventory: Optional[InventoryData] = None  # cache for thumbnail refresh

        # Portrait 3D system (separate renderer, shared params)
        self._portrait_vulkan: Optional[VulkanDepthRenderer] = None
        self._portrait_depth_key: Optional[str] = None
        self._loaded_portrait_depth_key: Optional[str] = None

        # Wire parser callbacks
        self.parser.on_room_update = self._on_room_update
        self.parser.on_combat = self._on_combat
        self.parser.on_hp_update = self._on_hp_update
        self.parser.on_death = self._on_death
        self.parser.on_xp = self._on_xp
        self.parser.on_inventory = self._on_inventory
        self.parser.on_who_list = self._on_who_list
        self.parser.on_char_name = self._on_char_name
        self.parser.on_chat = self._on_chat

        # Chat AI state
        self._chat_enabled = False
        self._chat_history: list[dict] = []  # [{sender, message, channel, timestamp}]
        self._chat_max_len = 250  # max gossip body length (platform limit ~251)
        self._chat_buffer: list[dict] = []  # buffered messages waiting to be processed
        self._chat_buffer_timer = None  # threading.Timer for batch processing
        self._chat_buffer_delay = 5.0  # seconds to wait for more messages before processing
        self._chat_last_reply_time = 0.0  # timestamp of last reply sent
        self._chat_cooldown = 8.0  # minimum seconds between replies
        self._chat_processing = False  # prevent overlapping batch processing

        # Combat target tracking — recent lines buffer for *Combat Engaged* lookback
        self._recent_lines: list[str] = []  # last ~10 stripped lines
        self._combat_target_hint: str = ""  # last known target from "moves to attack"

        # Wire proxy callbacks
        self.proxy.on_server_data_ansi = self._on_server_data_ansi
        self.proxy.on_server_data_bytes = self._on_server_data_bytes
        self.proxy.on_raw_line = self._on_raw_line
        self.proxy.on_connect = self._on_proxy_connect
        self.proxy.on_disconnect = self._on_proxy_disconnect

    def run(self) -> None:
        # Start asyncio in background thread
        self._loop = asyncio.new_event_loop()
        async_thread = threading.Thread(target=self._run_async, daemon=True)
        async_thread.start()

        # Wire GUI callbacks
        self.gui.set_inject_callback(self._gui_inject)
        self.gui.set_connect_callback(self._gui_connect)
        self.gui.set_disconnect_callback(self._gui_disconnect)
        self.gui.set_save_config_callback(self._gui_save_config)
        self.gui.set_grind_start_callback(self._gui_grind_start)
        self.gui.set_grind_stop_callback(self._gui_grind_stop)
        self.gui.set_room_view_callback(self._gui_toggle_room_view)
        self.gui.set_char_view_callback(self._gui_toggle_char_view)
        self.gui.set_depth_3d_callback(self._gui_depth_3d_toggled)
        self.gui.set_depth_params_callback(self._gui_depth_params_changed)
        self.gui.set_chatbot_callback(self._gui_chatbot_toggled)
        self.gui.set_portrait_style_callback(self._on_portrait_style_changed)

        # Load config into GUI after activation, and show room window + character window
        def _on_activate(_app):
            self.gui.load_config(self.config)
            self.gui.load_depth_config(self.config)
            self._room_window = RoomWindow()
            self._room_window.set_application(self.gui)
            self._room_window.set_regenerate_room_callback(self._regenerate_room)
            self._room_window.set_regenerate_entity_callback(self._regenerate_entity)
            self._room_window.set_entity_db(self._entity_db)
            self._room_window.load_view_config(self.config)
            self._room_window.present()

            self._room_window.set_inject_fn(self._gui_inject)
            self._room_window.set_3d_toggled_callback(self._gui_depth_3d_toggled)

            # Initialize Vulkan renderer (lazy — only on first 3D enable)
            self._room_window.set_3d_params(self._depth_params)

            self._char_window = CharacterWindow()
            self._char_window.set_application(self.gui)
            self._char_window.set_inject_fn(self._gui_inject)
            self._char_window.set_regenerate_entity_callback(self._regenerate_entity)
            self._char_window.set_3d_toggled_callback(self._portrait_3d_toggled)
            self._char_window.set_3d_params(self._depth_params)
            self._room_window.set_character_window(self._char_window)
            # Don't present yet — show when we have character data
        self.gui.connect('activate', _on_activate)

        # Run GTK (blocks)
        self.gui.run(sys.argv[:1])

        # Cleanup
        if self._vulkan_renderer:
            self._vulkan_renderer.shutdown()
        if self._portrait_vulkan:
            self._portrait_vulkan.shutdown()
        self._room_renderer.shutdown()
        asyncio.run_coroutine_threadsafe(self.proxy.stop(), self._loop)
        self._loop.call_soon_threadsafe(self._loop.stop)
        async_thread.join(timeout=3)

    def _run_async(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._start_services())
        self._loop.run_forever()

    async def _start_services(self) -> None:
        await self.proxy.start()
        self._cmd_api = CommandAPI(
            inject_fn=self.proxy.inject_command,
            get_state_fn=lambda: {
                "room": self._current_room,
                "hp": self._hp, "mana": self._mana,
                "connected": self.proxy.connected,
                "grind_running": self._autogrind._running,
                "grind_kills": self._autogrind.kills,
                "grind_xp": self._autogrind.xp_earned,
            },
            grind_start_fn=self._autogrind.start,
            grind_stop_fn=self._autogrind.stop,
        )
        await self._cmd_api.start()
        logger.info(f"Proxy ready on {self.config.proxy_host}:{self.config.proxy_port}")

        # Queue any entities that have prompts but missing thumbnails
        self._entity_db.queue_missing_thumbnails_on_startup()

    # --- GUI -> Proxy (main thread -> asyncio thread) ---

    def _gui_inject(self, text: str) -> None:
        asyncio.run_coroutine_threadsafe(
            self.proxy.inject_command(text), self._loop
        )

    def _gui_connect(self) -> None:
        self.gui.update_status("Connecting...")
        async def do_connect():
            if not self.proxy.connected:
                success = await self.proxy.connect_to_bbs()
                if not success:
                    self.gui.update_status("Connection failed")
        asyncio.run_coroutine_threadsafe(do_connect(), self._loop)

    def _gui_disconnect(self) -> None:
        async def do_disconnect():
            self.config.auto_reconnect = False
            await self.proxy._close_server_connection()
        asyncio.run_coroutine_threadsafe(do_disconnect(), self._loop)

    def _gui_grind_start(self, settings: dict) -> None:
        self._autogrind._flee_hp = settings.get('flee_hp', 10)
        self._autogrind._max_hp = settings.get('rest_to_hp', 24)
        self._autogrind.loot_copper = settings.get('loot_copper', True)
        self._autogrind.loot_silver = settings.get('loot_silver', True)
        self._autogrind.loot_gold = settings.get('loot_gold', True)
        self._autogrind.loot_platinum = settings.get('loot_platinum', True)
        self._autogrind.loot_runic = settings.get('loot_runic', True)
        asyncio.run_coroutine_threadsafe(self._autogrind.start(), self._loop)

    def _gui_grind_stop(self) -> None:
        asyncio.run_coroutine_threadsafe(self._autogrind.stop(), self._loop)

    def _gui_toggle_room_view(self, visible: bool) -> None:
        if not self._room_window:
            return
        if visible:
            self._room_window.present()
        else:
            self._room_window.set_visible(False)

    def _gui_toggle_char_view(self, visible: bool) -> None:
        if not self._char_window:
            return
        if visible:
            self._char_window.present()
            # Refresh inventory when opening
            self._request_inventory()
        else:
            self._char_window.set_visible(False)

    def _gui_save_config(self, gui_config: dict) -> None:
        self.config.bbs_host = gui_config['bbs_host']
        self.config.bbs_port = gui_config['bbs_port']
        self.config.username = gui_config['username']
        self.config.password = gui_config['password']
        self.config.auto_reconnect = gui_config['auto_reconnect']
        self.config.reconnect_delay = gui_config['reconnect_delay']
        self.proxy.config = self.config
        self.config.save()
        logger.info("Config saved")

    # --- Proxy -> GUI (asyncio thread -> main thread via GLib.idle_add) ---

    def _on_server_data_ansi(self, text: str) -> None:
        pass  # VTE handles display via raw bytes now

    def _on_server_data_bytes(self, data: bytes) -> None:
        self.gui.feed_raw_bytes(data)

    def _on_raw_line(self, line: str, ansi_line: str = '') -> None:
        stripped = line.rstrip()
        if stripped:
            logging.getLogger('mudproxy.raw').debug(stripped)

        # Feed to entity_db first — if it's collecting a look response, it consumes the line
        if self._entity_db.feed_look_line(line):
            return  # line was part of a look response, don't feed to parser

        # Track recent lines for combat target lookback
        if stripped:
            self._recent_lines.append(stripped)
            if len(self._recent_lines) > 15:
                self._recent_lines = self._recent_lines[-15:]

        # Combat target tracking
        if self._room_window:
            if '*Combat Off*' in stripped:
                self._room_window.clear_combat_target()
                self._combat_target_hint = ""
            elif '*Combat Engaged*' in stripped:
                self._detect_combat_target()
            # "X moves to attack Y" — track the target name
            moves_match = re.match(r'^\w+\s+moves to attack\s+(.+?)\.?$', stripped)
            if moves_match:
                target = moves_match.group(1)
                self._combat_target_hint = target
                self._room_window.set_combat_target(target)

        self.parser.feed_line(line, ansi_line)
        self._autogrind.feed_line(line)

    def _on_proxy_connect(self) -> None:
        self.gui.set_connected_state(True)
        self.gui.update_status("Connected")

        # Schedule initial stat + who after a delay (wait for login)
        async def _delayed_init():
            await asyncio.sleep(10)  # wait for login to complete
            if self.proxy.connected:
                # stat gives us our character name via "Name:" line
                await self.proxy.inject_command("stat")
                await asyncio.sleep(3)
                await self.proxy.inject_command("who")

        if self._loop:
            asyncio.run_coroutine_threadsafe(_delayed_init(), self._loop)

        # Periodic who every 5 minutes
        def _who_tick():
            if self.proxy.connected:
                self._request_who()
            return True  # keep timer running
        self._who_timer = GLib.timeout_add_seconds(300, _who_tick)

        # Periodic inventory refresh every 30 seconds
        def _inv_tick():
            if self.proxy.connected and not self._auto_looking:
                self._request_inventory()
            return True
        self._inventory_timer = GLib.timeout_add_seconds(30, _inv_tick)

    def _on_proxy_disconnect(self) -> None:
        self.gui.set_connected_state(False)
        self.gui.update_status("Disconnected")
        if self._who_timer:
            GLib.source_remove(self._who_timer)
            self._who_timer = None
        if self._inventory_timer:
            GLib.source_remove(self._inventory_timer)
            self._inventory_timer = None

    # --- Parser callbacks ---

    @staticmethod
    def _strip_player_perspective(desc: str) -> str:
        """Remove player-perspective phrasing from room description for image gen.
        Converts 'You are standing on a trail' style to just scene description."""
        import re
        lines = []
        for line in desc.split('\n'):
            # Replace "You are/can/notice" with neutral phrasing
            cleaned = re.sub(r'\bYou are standing\b', 'Standing', line)
            cleaned = re.sub(r'\bYou are\b', 'One is', cleaned)
            cleaned = re.sub(r'\byou are\b', 'one is', cleaned)
            cleaned = re.sub(r'\bYou can\b', 'One can', cleaned)
            cleaned = re.sub(r'\byou can\b', 'one can', cleaned)
            cleaned = re.sub(r'\byour\b', 'the', cleaned)
            cleaned = re.sub(r'\bYour\b', 'The', cleaned)
            cleaned = re.sub(r'\byou\b', 'one', cleaned)
            cleaned = re.sub(r'\bYou\b', 'One', cleaned)
            lines.append(cleaned)
        return '\n'.join(lines)

    def _on_room_update(self, room: RoomData) -> None:
        self._current_room = room.name
        self._look_fail_count.clear()  # reset failures on room change
        logger.info(f"Room: {room.name} | Exits: {room.exits} | Monsters: {room.monsters}")
        self.gui.update_room(
            name=room.name,
            description=room.description,
            exits=room.exits,
            items=room.items,
            monsters=room.monsters,
        )

        # Update room window text and request image generation
        if not self._room_window:
            return
        # Clean ANSI artifacts and wound status lines from description
        clean_desc = strip_ansi(room.description) if room.description else ""
        clean_desc = "\n".join(
            line for line in clean_desc.split("\n")
            if not self._entity_db._is_wound_status(line.strip())
        )

        # Always update the room text (name, monsters, exits, items)
        self._room_window.update_room_text(
            name=room.name,
            description=clean_desc,
            monsters=room.monsters,
            exits=room.exits,
            items=room.items,
        )

        # If no description or broken/too-short description, auto-look (only once per room)
        desc_broken = (
            not clean_desc.strip()
            or (len(clean_desc.strip()) < 30
                and clean_desc.strip().rstrip('.').lower() not in
                [e.lower() for e in room.exits])
        )
        if desc_broken and self._loop and room.name not in self._room_look_pending:
            self._room_look_pending.add(room.name)
            logger.info(f"Missing/broken description for {room.name}: {clean_desc!r}, auto-looking")
            asyncio.run_coroutine_threadsafe(
                self.proxy.inject_command("l"), self._loop
            )
            # Don't render yet — wait for the look to provide description
            return

        self._room_look_pending.discard(room.name)
        # Track cache key for depth processing
        new_key = self._room_renderer._cache_key(room.name, room.exits)
        if new_key != self._current_room_cache_key:
            self._current_room_cache_key = new_key
            self._loaded_depth_key = None  # Force re-upload for new room

            if self._room_window:
                has_2d = self._room_renderer._check_cache(new_key) is not None
                has_3d = (self._depth_processor.has_depth(new_key)
                          if self._depth_processor else False)

                if self._room_window._3d_enabled:
                    if has_3d:
                        # Depth cached — keep old scene running, new depth will
                        # be uploaded and crossfade handled by set_3d_scene_ready
                        self._room_window._3d_scene_ready = False
                    elif has_2d:
                        # Have 2D image but need depth — keep old 3D scene
                        # visible while depth generates, just update status
                        self._room_window._3d_scene_ready = False
                        self._room_window.update_status("Generating depth map...")
                    else:
                        # No image at all — show placeholder
                        self._room_window.invalidate_3d_scene()
                else:
                    if not has_2d:
                        self._room_window.show_generating()

        self._room_renderer.request_render(
            room_name=room.name,
            description=self._strip_player_perspective(clean_desc),
            monsters=room.monsters,
            items=room.items,
            exits=room.exits,
        )

        # Update entity thumbnails in room window
        try:
            if room.monsters:
                logger.info(f"Monsters in room: {room.monsters}")
                entity_info = self._entity_db.get_entity_display_info(room.monsters)
                self._room_window.update_thumbnails(
                    entity_info, self._entity_db.get_thumbnail
                )

                # Auto-look at new creatures we haven't seen before
                targets = self._entity_db.get_look_targets(room.monsters)
                logger.info(f"Auto-look targets: {targets}")
                if targets:
                    for t in targets:
                        self._auto_look_queue.append(("npc", t))
                    self._process_auto_look_queue()

                # Re-request thumbnails for entities that have descriptions but no thumbnail
                self._entity_db.request_missing_thumbnails(room.monsters)
            else:
                self._room_window.update_thumbnails([], self._entity_db.get_thumbnail)

            # Update item thumbnails
            if room.items:
                logger.info(f"Items in room: {room.items}")
                item_info = self._entity_db.get_item_display_info(
                    room.items, quantities=room.item_quantities)
                self._room_window.update_item_thumbnails(
                    item_info, self._entity_db.get_thumbnail
                )

                # Auto-look at items we haven't seen
                item_targets = self._entity_db.get_item_look_targets(room.items)
                if item_targets:
                    for t in item_targets:
                        self._auto_look_queue.append(("item", t))
                    self._process_auto_look_queue()

                self._entity_db.request_missing_item_thumbnails(room.items)
            else:
                self._room_window.update_item_thumbnails([], self._entity_db.get_thumbnail)
        except Exception as e:
            logger.error(f"Entity/thumbnail error: {e}", exc_info=True)

    def _on_entity_status(self, text: str) -> None:
        """Forward entity generation status to the room window."""
        if self._room_window:
            self._room_window.update_status(text)

    def _on_gen_progress(self, activity: str, progress: float) -> None:
        """Forward generation progress to the GUI and room console."""
        def _update():
            self.gui.update_generation_status(activity, progress)
            if self._room_window and progress >= 0:
                self._room_window.update_progress_bar("room_gen", activity, progress)
            elif self._room_window and activity:
                self._room_window.update_status(activity)
        GLib.idle_add(_update)

    def _on_thumb_progress(self, entity_key: str, fraction: float) -> None:
        """Forward per-thumbnail progress to both windows."""
        def _update():
            if self._room_window:
                self._room_window.update_thumb_progress(entity_key, fraction)
                self._room_window.update_progress_bar(
                    f"thumb_{entity_key}", f"Thumbnail: {entity_key}", fraction
                )
        GLib.idle_add(_update)
        if self._char_window:
            self._char_window.update_thumb_progress(entity_key, fraction)

    def _on_room_image_ready(self, room_name: str, prompt: str, png_bytes: bytes) -> None:
        """Called from renderer thread when image generation completes."""
        logger.info(f"Image ready for: {room_name} ({len(png_bytes)} bytes)")
        def _update():
            # Only display if this is still the current room
            if self._room_window and room_name == self._current_room:
                self._room_window.set_image(png_bytes)
            elif room_name != self._current_room:
                logger.info(f"Skipping stale image for '{room_name}' (current: '{self._current_room}')")
                return

            # Trigger depth processing if 3D mode is enabled
            if self.config.depth_3d_enabled:
                self._trigger_depth_for_current_room()
        GLib.idle_add(_update)

    def _on_thumbnail_ready(self, entity_key: str, png_bytes: bytes) -> None:
        """Called from renderer thread when a thumbnail finishes generating."""
        logger.info(f"Thumbnail ready for: {entity_key} ({len(png_bytes)} bytes)")
        self._entity_db.save_thumbnail(entity_key, png_bytes)

        def _update():
            # Refresh thumbnails in room window
            if self._room_window:
                if self.parser._room.monsters:
                    entity_info = self._entity_db.get_entity_display_info(
                        self.parser._room.monsters
                    )
                    self._room_window.update_thumbnails(
                        entity_info, self._entity_db.get_thumbnail
                    )
                if self.parser._room.items:
                    item_info = self._entity_db.get_item_display_info(
                        self.parser._room.items
                    )
                    self._room_window.update_item_thumbnails(
                        item_info, self._entity_db.get_thumbnail
                    )

            # Refresh character window equipment/inventory thumbnails
            if self._char_window and self._last_inventory:
                self._on_inventory(self._last_inventory)
        GLib.idle_add(_update)

        # Check if this is our character portrait
        if self._char_window and self._char_name and entity_key.lower() == self._char_name.lower():
            self._char_window.set_portrait(png_bytes)
            # Re-trigger depth processing if 3D portrait is active
            if self._char_window._3d_enabled:
                self._char_window.set_3d_scene_ready(False)
                self._loaded_portrait_depth_key = None
                self._trigger_portrait_depth()

    def _get_entity_description(self, name: str) -> Optional[str]:
        """Get a known entity's description for the room prompt builder."""
        _prefix, base = self._entity_db.parse_prefix_and_base(name)
        info = self._entity_db.get_entity(base)
        if info and info.description:
            return info.description
        return None

    def _regenerate_room(self) -> None:
        """Regenerate the current room image with a new random seed."""
        room = self.parser._room
        if not room.name:
            return
        logger.info(f"User requested room regeneration: {room.name}")
        clean_desc = strip_ansi(room.description) if room.description else ""
        clean_desc = "\n".join(
            line for line in clean_desc.split("\n")
            if not self._entity_db._is_wound_status(line.strip())
        )
        # Delete cached image and depth map so everything regenerates
        cache_key = self._room_renderer._cache_key(room.name, room.exits)
        self._room_renderer.invalidate_cache(room.name, room.exits)
        self._depth_processor.invalidate(cache_key)
        self._loaded_depth_key = None
        self._room_renderer.request_render(
            room_name=room.name,
            description=self._strip_player_perspective(clean_desc),
            monsters=room.monsters,
            items=room.items,
            exits=room.exits,
        )

    def _regenerate_entity(self, entity_name: str) -> None:
        """Regenerate a specific entity's thumbnail with a new seed."""
        logger.info(f"User requested entity regeneration: {entity_name}")
        _prefix, base = self._entity_db.parse_prefix_and_base(entity_name)
        info = self._entity_db.get_entity(base)
        if info and info.base_prompt:
            base_prompt = info.base_prompt
        else:
            # Fallback: generate prompt from name
            base_prompt = f"a {entity_name}, medieval fantasy RPG character"
            logger.info(f"No base_prompt for {entity_name}, using fallback")
        # Delete old thumbnail by base name (that's how they're stored)
        self._entity_db.delete_thumbnail(base)
        # Also delete prefixed variant in case it exists
        if entity_name.lower() != base.lower():
            self._entity_db.delete_thumbnail(entity_name)
        if info and info.description:
            # Re-run LLM prompt generation so style changes take effect
            info.base_prompt = ""
            self._entity_db._save_entity(info)
            import threading
            def _regen():
                self._entity_db._generate_entity_prompt(info)
            threading.Thread(target=_regen, daemon=True).start()
        else:
            # No description, use existing prompt with new seed
            import random
            seed_tag = f"seed:{random.randint(1, 999999)}"
            is_self = self._char_name and entity_name.lower() == self._char_name.lower()
            style = (self._entity_db.portrait_style or "fantasy art") if is_self else "fantasy art"
            if info and info.entity_type == "item":
                thumb_prompt = (f"{base_prompt}, single object laid flat on dark background, "
                                f"no person, no mannequin, no figure, item icon, fantasy art, detailed, {seed_tag}")
            else:
                thumb_prompt = f"{base_prompt}, portrait, square frame, {style}, detailed, {seed_tag}"
            self._room_renderer.generate_thumbnail(entity_name, thumb_prompt, "regen")

    def _process_auto_look_queue(self) -> None:
        """Inject 'l <target>' commands for creatures/items we haven't looked at yet."""
        if self._auto_looking or not self._auto_look_queue:
            if not self._auto_look_queue:
                self._room_window.update_status("Ready")
            return
        if not self._loop:
            return

        entry = self._auto_look_queue.pop(0)
        # Entries are (type, name) tuples
        if isinstance(entry, tuple):
            entity_type, target = entry
        else:
            entity_type, target = "npc", entry

        # Always strip quantity prefix from target (never send "l 50 runic coins")
        target = self._entity_db.strip_quantity(target)

        # Never look at currency or numeric names
        if self._entity_db._should_skip_item(target):
            logger.info(f"Skipping auto-look for {target} (currency/numeric)")
            if self._auto_look_queue:
                self._process_auto_look_queue()
            else:
                self._room_window.update_status("Ready")
            return

        # Skip if we've failed too many times
        key = target.lower()
        if self._look_fail_count.get(key, 0) >= self._look_max_retries:
            logger.info(f"Skipping {target} — failed {self._look_max_retries} times")
            if self._auto_look_queue:
                self._process_auto_look_queue()
            else:
                self._room_window.update_status("Ready")
            return

        remaining = len(self._auto_look_queue)
        self._room_window.update_status(f"Looking at: {target} ({remaining} queued)")

        self._auto_looking = True
        self._entity_db.start_look(target, entity_type=entity_type)

        async def _do_look():
            await self.proxy.inject_command(f"l {target}")
            # Wait for response to arrive
            await asyncio.sleep(0.3)
            # Check if we're still collecting (data arrived but no terminator)
            if self._entity_db._collecting_look and self._entity_db._look_target == target:
                if self._entity_db._look_buffer:
                    # Have data but no terminator — finalize what we got
                    logger.info(f"Look for {target}: finalizing with collected data (no terminator)")
                    self._entity_db._finalize_look()
                elif self._entity_db._look_waiting_for_echo or self._entity_db._look_waiting_for_response:
                    # No data at all — true failure
                    self._entity_db._collecting_look = False
                    self._entity_db._look_waiting_for_response = False
                    self._entity_db._look_waiting_for_echo = False
                    self._entity_db._look_target = None
                    self._entity_db._look_buffer = []
                    self._look_fail_count[key] = self._look_fail_count.get(key, 0) + 1
                    logger.warning(f"Look failed for {target} (attempt {self._look_fail_count[key]})")
            else:
                # Already finalized — success
                pass
            self._auto_looking = False
            if self._auto_look_queue:
                self._process_auto_look_queue()
            else:
                self._room_window.update_status("Ready")

        asyncio.run_coroutine_threadsafe(_do_look(), self._loop)

    def _on_inventory(self, inv: InventoryData) -> None:
        """Called when parser detects inventory output."""
        logger.info(f"Inventory: {len(inv.items)} items, wealth={inv.wealth}")
        self._last_inventory = inv
        if not self._char_window:
            return

        # Equipped: items with slots (vertical list with thumbnails)
        equipped = []
        for item in inv.items:
            if item.get("slot"):
                equipped.append({
                    "name": item["name"],
                    "slot": item["slot"],
                    "thumb_key": item["name"],
                    "has_thumb": self._entity_db.has_thumbnail(item["name"]),
                })
        self._char_window.update_equipment(equipped, self._entity_db.get_thumbnail)

        # Inventory: paragraph flow "You are carrying..." with inline thumbnails
        all_inv_items = []
        for item in inv.items:
            all_inv_items.append({
                "name": item["name"],
                "slot": item.get("slot", ""),
                "thumb_key": item["name"],
                "has_thumb": self._entity_db.has_thumbnail(item["name"]),
            })
        self._char_window.update_inventory(
            all_inv_items, self._entity_db.get_thumbnail,
            wealth=inv.wealth, encumbrance=inv.encumbrance,
        )

        # Auto-look at items we haven't seen or need verification
        for item in inv.items:
            name = item["name"]
            if self._entity_db._should_skip_item(name):
                continue
            base = self._entity_db.strip_quantity(name)
            if self._entity_db._should_skip_item(base):
                continue
            needs_verify = base.lower() in self._entity_db._candidate_descriptions
            if not self._entity_db.has_description(base) or needs_verify:
                retry_delay = 10 if needs_verify else 30
                last = self._entity_db._pending_looks.get(base.lower(), 0)
                if time.time() - last > retry_delay:
                    self._auto_look_queue.append(("item", base))
                    self._entity_db._pending_looks[base.lower()] = time.time()
        if self._auto_look_queue:
            self._process_auto_look_queue()

    def _on_char_name(self, full_name: str) -> None:
        """Called when parser detects 'Name: ...' from stat output."""
        first_name = full_name.split()[0] if full_name else ""
        if not first_name:
            return
        logger.info(f"Character name from stat: {full_name} (first: {first_name})")
        self._char_name = first_name
        self._char_full_name = full_name
        self._entity_db.my_char_name = first_name
        self.config.character_name = first_name
        self.config.save()

        # Now look at ourselves for portrait + description
        async def _do():
            await asyncio.sleep(2)
            self._request_self_look()
            await asyncio.sleep(3)
            self._request_inventory()
        if self._loop:
            asyncio.run_coroutine_threadsafe(_do(), self._loop)

    def _on_chat(self, sender: str, message: str, channel: str) -> None:
        """Called when parser detects an incoming chat message."""
        import time as _time
        import threading
        logger.info(f"Chat [{channel}] {sender}: {message}")
        entry = {
            "sender": sender,
            "message": message,
            "channel": channel,
            "timestamp": _time.time(),
        }
        self._chat_history.append(entry)
        if len(self._chat_history) > 50:
            self._chat_history = self._chat_history[-50:]

        if not self._chat_enabled:
            return

        # Ignore MegaMud @commands (remote control: @ok, @heal, @health, etc.)
        if message.strip().startswith('@'):
            logger.debug(f"Ignoring MegaMud @command from {sender}: {message}")
            return

        # Buffer the message — reset the timer each time so we batch rapid messages
        self._chat_buffer.append(entry)
        if self._chat_buffer_timer is not None:
            self._chat_buffer_timer.cancel()
        self._chat_buffer_timer = threading.Timer(
            self._chat_buffer_delay, self._process_chat_buffer
        )
        self._chat_buffer_timer.start()

    def _process_chat_buffer(self) -> None:
        """Process buffered chat messages with 2-pass system."""
        import time as _time
        if self._chat_processing:
            return
        self._chat_processing = True

        try:
            # Grab and clear buffer
            batch = self._chat_buffer[:]
            self._chat_buffer.clear()
            if not batch:
                return

            # Cooldown check
            elapsed = _time.time() - self._chat_last_reply_time
            if elapsed < self._chat_cooldown:
                logger.info(f"Chat cooldown ({elapsed:.0f}s < {self._chat_cooldown}s), skipping")
                return

            # Determine which channel to reply on (most recent message's channel)
            channel = batch[-1]["channel"]

            # Build conversation context
            history_lines = []
            for h in self._chat_history[-20:]:
                history_lines.append(f"{h['sender']}: {h['message']}")
            history_text = "\n".join(history_lines)

            # New messages in this batch
            new_lines = []
            for b in batch:
                new_lines.append(f"{b['sender']}: {b['message']}")
            new_text = "\n".join(new_lines)

            # === PASS 1: Should we respond? (fast model) ===
            should_respond = self._chat_should_respond(history_text, new_text, batch)
            if not should_respond:
                logger.info("Pass 1: decided NOT to respond")
                return

            logger.info("Pass 1: decided to respond")

            # === PASS 2: Generate the actual reply (quality model) ===
            reply = self._chat_generate_reply(history_text, new_text, channel)
            if not reply:
                return

            # Send it
            reply = reply[:self._chat_max_len]
            # Pick the right command for the channel
            if channel == "gangpath":
                cmd = f"bg {reply}"
            elif channel == "broadcast":
                cmd = f"br {reply}"
            elif channel == "gossip":
                cmd = f"gos {reply}"
            elif channel == "telepath":
                sender = batch[-1]["sender"]
                cmd = f"/{sender} {reply}"
            elif channel == "say":
                # Direct message to the speaker if in room
                sender = batch[-1]["sender"]
                cmd = f">{sender} {reply}"
            else:
                return

            logger.info(f"Chat reply [{channel}] -> {reply}")
            self._chat_last_reply_time = _time.time()
            if self._loop:
                import asyncio
                asyncio.run_coroutine_threadsafe(
                    self.proxy.inject_command(cmd), self._loop
                )
        except Exception as e:
            logger.error(f"Chat buffer processing error: {e}", exc_info=True)
        finally:
            self._chat_processing = False

    def _chat_should_respond(self, history: str, new_msgs: str, batch: list) -> bool:
        """Pass 1: Rule-based filter — should we respond?"""
        import time as _time

        # Always respond if someone mentions our name
        name_lower = self.parser._char_name_lower or "tripmunk"
        for b in batch:
            if name_lower in b["message"].lower():
                logger.info("Pass 1: YES — name mentioned")
                return True

        # Always respond to direct say (someone in the room talking)
        if batch[-1]["channel"] == "say":
            logger.info("Pass 1: YES — room say")
            return True

        # Always respond to telepaths (private messages)
        if batch[-1]["channel"] == "telepath":
            logger.info("Pass 1: YES — telepath")
            return True

        # For gossip/gangpath/broadcast: respond if conversation is slow
        # (only 1-2 unique speakers in batch = not a rapid-fire group convo)
        speakers = set(b["sender"] for b in batch)
        if len(speakers) <= 2:
            logger.info(f"Pass 1: YES — {len(speakers)} speaker(s)")
            return True

        # Many speakers talking at once — skip unless mentioned
        logger.info(f"Pass 1: NO — {len(speakers)} speakers rapid-fire")
        return False

    def _chat_generate_reply(self, history: str, new_msgs: str, channel: str) -> str:
        """Pass 2: Quality model generates the actual reply."""
        import requests

        system_prompt = (
            "You are in a group chat. Your name is Tripmunk. "
            "Read the conversation and reply naturally to what people are ACTUALLY talking about. "
            "STAY ON TOPIC. Do not change the subject. Do not bring up random stuff.\n\n"
            "RULES:\n"
            "- NO emojis ever\n"
            "- SHORT replies. 5-15 words. like texting\n"
            "- lowercase, minimal punctuation\n"
            "- respond to what was said, not what you wish was said\n"
            "- if you have nothing relevant to add, just react ('haha', 'damn', 'nice', 'yeah for real')\n"
            "- never say generic filler like 'happy hunting' or 'safe travels'\n"
            "- never give unsolicited advice\n"
            "- never reveal you are AI\n"
            "- output ONLY the message. no quotes, no prefix, no 'Tripmunk:'"
        )

        user_msg = f"Chat log:\n{history}\n\nNew messages:\n{new_msgs}\n\nYour reply (short, natural):"

        try:
            resp = requests.post(
                "http://localhost:11434/api/chat",
                json={
                    "model": "deepseek-v2:16b",
                    "messages": [
                        {"role": "system", "content": system_prompt},
                        {"role": "user", "content": user_msg},
                    ],
                    "stream": False,
                    "options": {"num_predict": 60, "temperature": 0.8},
                },
                timeout=60,
            )
            resp.raise_for_status()
            reply = resp.json()["message"]["content"].strip()
            logger.info(f"Pass 2 reply: {reply[:300]}")

            # Strip quotes if the model wraps the reply
            if reply.startswith('"') and reply.endswith('"'):
                reply = reply[1:-1]
            # Strip any "Tripmunk:" prefix the model might add
            for prefix in ("Tripmunk:", "Tripmunk gossips:", "tripmunk:", "tripmunk gossips:"):
                if reply.lower().startswith(prefix.lower()):
                    reply = reply[len(prefix):].strip()
            return reply[:self._chat_max_len]
        except Exception as e:
            logger.error(f"Pass 2 error: {e}", exc_info=True)
            return ""

    def _on_who_list(self, players: list[str]) -> None:
        """Called when parser detects 'who' output."""
        logger.info(f"Who list: {players}")
        self._entity_db.update_known_players(players)

    def _on_look_complete(self, target: str, info) -> None:
        """Called when entity_db finishes processing a look response."""
        # If this is our character, update the character window
        if self._char_name and target.lower() == self._char_name.lower():
            full_name = getattr(info, 'full_name', None) or self._char_name
            logger.info(f"Self-look complete for {target}: {info.description[:80]}...")

            # Update char name if we got it from brackets
            if full_name and full_name != self._char_name:
                self._char_name = info.name  # base name for lookups
                self.config.character_name = info.name
                self.config.save()

            if self._char_window:
                # Extract description (before "equipped with" section)
                desc = info.description
                if "equipped with" in desc.lower():
                    desc = desc[:desc.lower().index("equipped with")].strip()
                    for suffix in ["He is", "She is", "It is", "They are"]:
                        if desc.endswith(suffix):
                            desc = desc[:-len(suffix)].strip()

                # Load existing portrait if available
                portrait_bytes = self._entity_db.get_thumbnail(info.name)
                self._char_window.update_character(
                    name=full_name,
                    description=desc,
                    portrait_bytes=portrait_bytes,
                )

                # Only generate if no portrait exists at all (never auto-regen)
                if not portrait_bytes:
                    if info.base_prompt or info.description:
                        logger.info("Generating initial character portrait")
                        import threading
                        threading.Thread(
                            target=self._entity_db._generate_entity_prompt,
                            args=(info,), daemon=True
                        ).start()

                # Show character window
                if not self._char_window.get_visible():
                    self._char_window.present()

    def _request_self_look(self) -> None:
        """Send 'l <charname>' to get our character's description for portrait."""
        if not self._char_name or not self._loop:
            return
        # Use entity_db look collector
        self._entity_db.start_look(self._char_name, entity_type="player")
        asyncio.run_coroutine_threadsafe(
            self.proxy.inject_command(f"l {self._char_name}"), self._loop
        )

    def _request_inventory(self) -> None:
        """Send 'i' to refresh inventory."""
        if not self._loop or not self.proxy.connected:
            return
        asyncio.run_coroutine_threadsafe(
            self.proxy.inject_command("i"), self._loop
        )

    def _request_who(self) -> None:
        """Send 'who' to refresh player list."""
        if not self._loop or not self.proxy.connected:
            return
        asyncio.run_coroutine_threadsafe(
            self.proxy.inject_command("who"), self._loop
        )

    # --- 3D Depth callbacks ---

    def _on_portrait_style_changed(self, style: str) -> None:
        """Update portrait art style on entity DB and persist to config."""
        self._entity_db.portrait_style = style
        self.config.portrait_style = style
        self.config.save()
        logger.info(f"Portrait style set to: {style!r}")

    def _portrait_3d_toggled(self, enabled: bool) -> None:
        """Toggle 3D mode for the character portrait."""
        logger.info(f"Portrait 3D: {'ON' if enabled else 'OFF'}")

        if enabled and not self._portrait_vulkan:
            # Create a separate 384x384 Vulkan renderer for the portrait
            self._portrait_vulkan = VulkanDepthRenderer(width=384, height=384)
            if self._portrait_vulkan.available:
                logger.info("Portrait Vulkan renderer initialized (384x384)")
                self._char_window.set_3d_renderer(self._portrait_vulkan)
                # Queue depth processing for current portrait
                self._trigger_portrait_depth()
            else:
                logger.warning("Portrait Vulkan renderer not available")

        if enabled and self._portrait_vulkan and self._portrait_vulkan.available:
            if not self._char_window._3d_renderer:
                self._char_window.set_3d_renderer(self._portrait_vulkan)
            self._trigger_portrait_depth()

    def _trigger_portrait_depth(self) -> None:
        """Queue depth processing for the current portrait image."""
        if not self._char_window or not self._char_window._portrait_bytes:
            return

        import hashlib
        import tempfile
        portrait_bytes = self._char_window._portrait_bytes
        key = "portrait_" + hashlib.sha256(portrait_bytes).hexdigest()[:16]
        self._portrait_depth_key = key

        # Check if already loaded
        if key == self._loaded_portrait_depth_key:
            self._char_window.set_3d_scene_ready(True)
            return

        # Write portrait to a temp file for depth processor
        tmp = Path(tempfile.gettempdir()) / f"mudproxy_portrait_{key}.webp"
        if not tmp.exists():
            tmp.write_bytes(portrait_bytes)

        # Use depth processor with a portrait-specific callback
        if self._depth_processor.has_depth(key):
            depth_path, inpaint_path = self._depth_processor.get_cached_paths(key)
            self._on_portrait_depth_ready(key, str(tmp), depth_path, inpaint_path)
        else:
            # Queue — reuse processor but with custom callback wrapper
            old_cb = self._depth_processor._on_depth_ready
            def _portrait_cb(rk, cp, dp, ip):
                if rk.startswith("portrait_"):
                    self._on_portrait_depth_ready(rk, cp, dp, ip)
                else:
                    old_cb(rk, cp, dp, ip)
            self._depth_processor._on_depth_ready = _portrait_cb
            self._depth_processor.process_image(key, str(tmp))

    def _on_portrait_depth_ready(self, key: str, color_path: str,
                                  depth_path: Optional[str],
                                  inpaint_path: Optional[str]) -> None:
        """Depth map ready for portrait — upload to portrait GPU."""
        if not self._portrait_vulkan or not self._portrait_vulkan.available:
            return
        if key != self._portrait_depth_key:
            return
        if key == self._loaded_portrait_depth_key:
            return

        try:
            from PIL import Image

            color_img = Image.open(color_path).convert("RGBA").resize((384, 384))
            color_rgba = color_img.tobytes()

            depth_img = Image.open(depth_path).convert("L").resize((384, 384))
            depth_gray = depth_img.tobytes()

            inpaint_rgba = None
            if inpaint_path:
                inp_img = Image.open(inpaint_path).convert("RGBA").resize((384, 384))
                inpaint_rgba = inp_img.tobytes()

            def _upload():
                # Pause portrait animation during upload
                if self._char_window._3d_timer is not None:
                    self._char_window._stop_3d_animation()

                self._portrait_vulkan.load_scene(
                    color_rgba, 384, 384,
                    depth_gray, 384, 384,
                    inpaint_rgba, 384 if inpaint_rgba else 0,
                    384 if inpaint_rgba else 0,
                )
                self._loaded_portrait_depth_key = key
                logger.info("Portrait depth scene loaded to Vulkan")
                self._char_window.set_3d_scene_ready(True)
                return False

            GLib.idle_add(_upload)

        except Exception as e:
            logger.error(f"Failed to load portrait depth scene: {e}", exc_info=True)

    def _gui_chatbot_toggled(self, enabled: bool) -> None:
        """Toggle chatbot mode — blocks FLUX when active."""
        logger.info(f"Chatbot mode: {'ON' if enabled else 'OFF'}")
        self._chat_enabled = enabled
        if enabled:
            # Block FLUX and unload it to free VRAM for deepseek
            self._room_renderer.blocked = True
            self._room_renderer.shutdown()
            logger.info("FLUX unloaded for chatbot mode")
        else:
            # Unload ALL Ollama models from VRAM first, then start FLUX
            def _unload_and_start_flux():
                try:
                    import requests
                    # Get all loaded models and unload each one
                    ps = requests.get("http://localhost:11434/api/ps", timeout=5).json()
                    for m in ps.get("models", []):
                        model_name = m.get("name", "")
                        requests.post(
                            "http://localhost:11434/api/generate",
                            json={"model": model_name, "keep_alive": 0},
                            timeout=10,
                        )
                        logger.info(f"Unloaded {model_name} from Ollama VRAM")
                except Exception as e:
                    logger.warning(f"Failed to unload Ollama models: {e}")
                # Now safe to load FLUX
                import time, gc, torch
                time.sleep(2)  # pause for VRAM to fully free
                gc.collect()
                torch.cuda.empty_cache()
                self._room_renderer.blocked = False
                self._room_renderer.load_pipeline()
                logger.info("FLUX loaded after chatbot disabled")
                # Re-queue any thumbnails that were skipped while chatbot was active
                self._entity_db.queue_missing_thumbnails_on_startup()

            import threading
            threading.Thread(target=_unload_and_start_flux, daemon=True).start()

    def _gui_depth_3d_toggled(self, enabled: bool) -> None:
        """Toggle 3D mode on/off."""
        logger.info(f"3D depth mode: {'ON' if enabled else 'OFF'}")
        self.config.depth_3d_enabled = enabled
        self._room_renderer.depth_3d_enabled = enabled
        self.config.save()

        if enabled and not self._vulkan_renderer:
            # Lazy-init the Vulkan renderer
            self._vulkan_renderer = VulkanDepthRenderer(width=1024, height=768)
            if self._room_window:
                self._room_window.set_3d_renderer(self._vulkan_renderer)

            if self._vulkan_renderer.available:
                logger.info("Vulkan depth renderer initialized")
                # Process current room if we have an image
                self._trigger_depth_for_current_room()
            else:
                logger.warning("Vulkan depth renderer not available")

        if self._room_window:
            # Set renderer before enabling (in case toggle came from GUI panel)
            if self._vulkan_renderer and not self._room_window._3d_renderer:
                self._room_window.set_3d_renderer(self._vulkan_renderer)
            self._room_window.set_3d_enabled(enabled)
            # Explicitly start/stop animation after renderer is guaranteed set
            if enabled:
                self._room_window._start_3d_animation()
            else:
                self._room_window._stop_3d_animation()

    def _gui_depth_params_changed(self, params_dict: dict) -> None:
        """Camera settings changed in GUI."""
        self._depth_params = DepthRenderParams.from_dict(params_dict)
        self.config.depth_3d_params = params_dict
        self.config.save()
        if self._room_window:
            self._room_window.set_3d_params(self._depth_params)
        if self._char_window:
            self._char_window.set_3d_params(self._depth_params)

    def _is_renderer_idle(self) -> bool:
        """Return True when room renderer has no pending room or thumbnail work."""
        r = self._room_renderer
        return (
            r._current_cache_key is None
            and not r._room_queue
            and not r._thumb_queue
            and (r._thumb_thread is None or not r._thumb_thread.is_alive())
        )

    def _trigger_depth_for_current_room(self) -> None:
        """Trigger depth processing for the current room image if available."""
        room = self.parser._room
        if not room.name:
            return
        from .room_renderer import CACHE_DIR as ROOM_CACHE_DIR
        cache_key = self._room_renderer._cache_key(room.name, room.exits)
        image_path = ROOM_CACHE_DIR / f"{cache_key}.webp"
        if image_path.exists():
            self._current_room_cache_key = cache_key
            self._depth_processor.process_image(cache_key, str(image_path))

    def _on_depth_ready(self, room_key: str, color_path: str,
                        depth_path: Optional[str], inpaint_path: Optional[str]) -> None:
        """Called from depth processor thread when depth map + inpaint are ready."""
        if not self._vulkan_renderer or not self._vulkan_renderer.available:
            return
        if room_key != self._current_room_cache_key:
            return  # Stale — different room now
        if room_key == self._loaded_depth_key:
            return  # Already loaded on GPU, skip

        try:
            from PIL import Image

            # Load images on this background thread (IO-bound)
            color_img = Image.open(color_path).convert("RGBA")
            color_rgba = color_img.tobytes()
            cw, ch = color_img.size

            depth_img = Image.open(depth_path).convert("L")
            depth_gray = depth_img.tobytes()
            dw, dh = depth_img.size

            inpaint_rgba = None
            iw, ih = 0, 0
            if inpaint_path:
                inp_img = Image.open(inpaint_path).convert("RGBA")
                inpaint_rgba = inp_img.tobytes()
                iw, ih = inp_img.size

            # Upload to GPU on main thread to avoid Vulkan race with render tick
            def _upload_on_main():
                if room_key != self._current_room_cache_key:
                    return False  # Room changed while queued

                # Pause animation during upload (vkDeviceWaitIdle)
                if self._room_window and self._room_window._3d_timer is not None:
                    self._room_window._stop_3d_animation(restore_static=False)

                self._vulkan_renderer.load_scene(
                    color_rgba, cw, ch,
                    depth_gray, dw, dh,
                    inpaint_rgba, iw, ih,
                )
                self._loaded_depth_key = room_key
                logger.info(f"Scene loaded to Vulkan: {cw}x{ch}")

                # Mark scene ready and start animation
                if self._room_window:
                    self._room_window.set_3d_scene_ready(True)
                return False

            GLib.idle_add(_upload_on_main)

        except Exception as e:
            logger.error(f"Failed to load depth scene: {e}", exc_info=True)

    # Regex to extract target and damage from combat lines
    # Check most-specific first: surprise > crit > normal
    _RE_SURPRISE_DAMAGE = re.compile(
        r'^(?:You|(\w+))\s+surprise\s+\w+\s+(.+?)\s+for\s+(\d+)\s+damage!',
        re.IGNORECASE
    )
    _RE_CRIT_DAMAGE = re.compile(
        r'^(?:You|(\w+))\s+critically\s+\w+\s+(.+?)\s+for\s+(\d+)\s+damage!',
        re.IGNORECASE
    )
    _RE_PLAYER_DAMAGE = re.compile(
        r'^(?:You|(\w+))\s+\w+\s+(.+?)\s+for\s+(\d+)\s+damage!',
        re.IGNORECASE
    )
    _RE_MONSTER_DEATH = re.compile(
        r'^(?:The\s+)?(.+?)\s+(?:drops?\s+dead|is\s+killed|collapses|crumples|'
        r'falls?\s+to\s+the\s+ground)',
        re.IGNORECASE
    )

    def _on_combat(self, line: str) -> None:
        self.gui.add_combat_line(line)

        if self._room_window:
            # Floating damage numbers — check most specific first
            surprise = self._RE_SURPRISE_DAMAGE.match(line)
            crit = not surprise and self._RE_CRIT_DAMAGE.match(line)
            dmg = surprise or crit or self._RE_PLAYER_DAMAGE.match(line)
            if dmg:
                attacker = dmg.group(1)  # None for "You", name for others
                target = dmg.group(2)
                amount = int(dmg.group(3))
                if attacker:
                    # Other player (not in party) — small dull grey
                    self._room_window.show_damage(target, amount,
                                                  color=(0.5, 0.5, 0.5))
                else:
                    # Your own hit — red, or crit/surprise effects
                    self._room_window.show_damage(target, amount,
                                                  crit=bool(crit),
                                                  surprise=bool(surprise))

            # Death detection handled by XP gain → room update comparison

    def _detect_combat_target(self) -> None:
        """On *Combat Engaged*, figure out who we're attacking by looking
        back at recent lines for a command targeting a known monster."""
        if not self._room_window or not self._current_room:
            return

        # Get current monster names in the room (lowercase)
        monsters = [m.lower() for m in (self.parser._room.monsters or [])]
        if not monsters:
            return

        # If only 1 monster, that's the target
        if len(monsters) == 1:
            self._combat_target_hint = monsters[0]
            self._room_window.set_combat_target(monsters[0])
            return

        # Look back through recent lines for a command like "[HP=x/MA=y]:bash cave bear"
        # The command is after the HP prompt colon
        for line in reversed(self._recent_lines):
            hp_match = re.search(r'\[HP=\d+/MA=\d+\]:\s*(.+)', line)
            if not hp_match:
                continue
            cmd = hp_match.group(1).strip().lower()
            # Check if any monster name appears in the command
            for mon in monsters:
                if mon in cmd:
                    self._combat_target_hint = mon
                    self._room_window.set_combat_target(mon)
                    return

        # Fallback: use "moves to attack" hint if we have one
        if self._combat_target_hint:
            self._room_window.set_combat_target(self._combat_target_hint)

    def _on_hp_update(self, hp: int, max_hp: int, mana: int, max_mana: int) -> None:
        self._hp = hp
        self._mana = mana
        self.gui.update_status(f"Connected | HP: {hp}/{max_hp} | Mana: {mana}/{max_mana}")

    def _on_death(self) -> None:
        self.gui.add_combat_line("*** YOU DIED ***")
        self.gui.update_status("DEAD")

    def _on_xp(self, amount: int) -> None:
        self.gui.add_combat_line(f"[+{amount} XP]")
        # Flag that a monster died — next room update will shatter the missing thumbnail
        if self._room_window:
            self._room_window.notify_xp_gained()


def main():
    app = MudProxyApp()
    app.run()


if __name__ == '__main__':
    main()
