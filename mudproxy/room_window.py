"""Floating GTK4 window that displays AI-generated room art with cross-fade.

Shows room name, description text, monsters, and a generated image
that cross-fades smoothly when the room changes.
Supports optional 3D depth parallax mode via Vulkan compute.
"""
import colorsys
import logging
import math
import subprocess
import threading
import time as _time
from pathlib import Path
from typing import Optional, Callable

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
from gi.repository import Gtk, Gdk, GLib, GdkPixbuf, Adw, Pango, Gio, Graphene

from .hold_menu import HoldMenu
from .combat_effects import CombatEffects

logger = logging.getLogger(__name__)

WINDOW_WIDTH = 780
WINDOW_HEIGHT = 720
IMAGE_HEIGHT = 512
CROSSFADE_DURATION_MS = 600
CROSSFADE_STEPS = 30


class RoomWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="MajorSLOP! — Room View")
        self.set_default_size(WINDOW_WIDTH, WINDOW_HEIGHT)
        self.set_resizable(True)
        self.set_decorated(True)

        # Keep on top
        self.set_modal(False)

        # Dark theme
        style_manager = Adw.StyleManager.get_default()
        style_manager.set_color_scheme(Adw.ColorScheme.FORCE_DARK)

        # --- Layout ---
        self._root_overlay = Gtk.Overlay()
        self.set_child(self._root_overlay)
        self._vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self._root_overlay.set_child(self._vbox)

        # Floating panel drag state — pending position applied once per frame
        self._drag_pending_panel = None
        self._drag_pending_x = 0
        self._drag_pending_y = 0
        self._drag_tick_id = None

        # Hold-right-click context menu
        self._hold_menu = HoldMenu(self._root_overlay)
        self._hold_menu.on_menu_open = self._on_hold_menu_open
        self._hold_menu.on_menu_close = self._on_hold_menu_close

        # HeaderBar inside content area (not as titlebar) so combat effects
        # DrawingArea overlay can render floating numbers over it.
        # Hide the real window titlebar with an invisible placeholder.
        _hide_titlebar = Gtk.Box()
        _hide_titlebar.set_visible(False)
        self.set_titlebar(_hide_titlebar)

        self._headerbar = Gtk.HeaderBar()
        self._headerbar.set_show_title_buttons(True)
        # Wrap in WindowHandle so dragging the bar moves the window
        self._header_handle = Gtk.WindowHandle()
        self._header_handle.set_child(self._headerbar)
        self._vbox.prepend(self._header_handle)

        self._room_label = Gtk.Label(label="Awaiting room data...")
        self._room_label.set_markup("<span size='large' weight='bold'>Awaiting room data...</span>")
        self._headerbar.set_title_widget(self._room_label)

        # Scanline thickness (stored as int, no widget)
        self._scanline_thickness = 2

        # User visibility flags (menu toggles)
        self._show_console = True
        self._show_monsters = True
        self._show_items = True

        # Build View menu with Gio.Menu + stateful actions
        view_menu = Gio.Menu()

        effects_section = Gio.Menu()
        effects_section.append("3D Mode", "view.toggle-3d")
        effects_section.append("Scanlines", "view.toggle-scanlines")
        effects_section.append("Warp Zoom", "view.toggle-warp")
        view_menu.append_section("Effects", effects_section)

        # Scanline thickness sub-menu
        thickness_menu = Gio.Menu()
        for px in range(1, 9):
            thickness_menu.append(f"{px}px", f"view.scanline-thickness({px})")
        view_menu.append_submenu("Scanline Thickness", thickness_menu)

        show_section = Gio.Menu()
        show_section.append("Console", "view.toggle-console")
        show_section.append("Monsters", "view.toggle-monsters")
        show_section.append("Items", "view.toggle-items")
        view_menu.append_section("Show", show_section)

        # NPC Display Location sub-menu (radio group)
        npc_loc_menu = Gio.Menu()
        npc_loc_menu.append("Above Image", "view.npc-location('above')")
        npc_loc_menu.append("Below Image", "view.npc-location('below')")
        npc_loc_menu.append("Floating", "view.npc-location('floating')")
        view_menu.append_submenu("NPC Display Location", npc_loc_menu)

        # Loot Display Location sub-menu (radio group)
        loot_loc_menu = Gio.Menu()
        loot_loc_menu.append("Above Image", "view.loot-location('above')")
        loot_loc_menu.append("Below Image", "view.loot-location('below')")
        loot_loc_menu.append("Floating", "view.loot-location('floating')")
        view_menu.append_submenu("Loot Display Location", loot_loc_menu)

        lock_section = Gio.Menu()
        lock_section.append("Lock NPC Panel", "view.toggle-npc-lock")
        lock_section.append("Lock Loot Panel", "view.toggle-loot-lock")
        view_menu.append_section(None, lock_section)

        # NPC Display Scale sub-menu
        npc_scale_menu = Gio.Menu()
        for pct in ("50%", "75%", "100%", "125%", "150%", "200%"):
            npc_scale_menu.append(pct, f"view.npc-scale('{pct}')")
        view_menu.append_submenu("NPC Display Scale", npc_scale_menu)

        # Loot Display Scale sub-menu
        loot_scale_menu = Gio.Menu()
        for pct in ("50%", "75%", "100%", "125%", "150%", "200%"):
            loot_scale_menu.append(pct, f"view.loot-scale('{pct}')")
        view_menu.append_submenu("Loot Display Scale", loot_scale_menu)

        # Damage Text Scale sub-menu
        dmg_scale_menu = Gio.Menu()
        for pct in ("50%", "75%", "100%", "125%", "150%", "200%"):
            dmg_scale_menu.append(pct, f"view.dmg-scale('{pct}')")
        view_menu.append_submenu("Damage Text Scale", dmg_scale_menu)

        # Zoomed Damage Text Scale sub-menu
        zdmg_scale_menu = Gio.Menu()
        for pct in ("50%", "75%", "100%", "125%", "150%", "200%"):
            zdmg_scale_menu.append(pct, f"view.zdmg-scale('{pct}')")
        view_menu.append_submenu("Zoomed Damage Text Scale", zdmg_scale_menu)

        view_btn = Gtk.MenuButton(label="View")
        view_btn.set_menu_model(view_menu)
        self._headerbar.pack_start(view_btn)

        # Register stateful toggle actions via action group
        self._view_actions = Gio.SimpleActionGroup()
        self.insert_action_group("view", self._view_actions)

        act_3d = Gio.SimpleAction.new_stateful(
            "toggle-3d", None, GLib.Variant.new_boolean(False))
        act_3d.connect("change-state", self._on_action_3d)
        self._view_actions.add_action(act_3d)

        act_scanlines = Gio.SimpleAction.new_stateful(
            "toggle-scanlines", None, GLib.Variant.new_boolean(False))
        act_scanlines.connect("change-state", self._on_action_scanlines)
        self._view_actions.add_action(act_scanlines)

        act_warp = Gio.SimpleAction.new_stateful(
            "toggle-warp", None, GLib.Variant.new_boolean(True))
        act_warp.connect("change-state", self._on_action_warp)
        self._view_actions.add_action(act_warp)

        act_thickness = Gio.SimpleAction.new_stateful(
            "scanline-thickness",
            GLib.VariantType.new("i"),
            GLib.Variant.new_int32(2))
        act_thickness.connect("change-state", self._on_action_thickness)
        self._view_actions.add_action(act_thickness)

        act_console = Gio.SimpleAction.new_stateful(
            "toggle-console", None, GLib.Variant.new_boolean(True))
        act_console.connect("change-state", self._on_action_console)
        self._view_actions.add_action(act_console)

        act_monsters = Gio.SimpleAction.new_stateful(
            "toggle-monsters", None, GLib.Variant.new_boolean(True))
        act_monsters.connect("change-state", self._on_action_monsters)
        self._view_actions.add_action(act_monsters)

        act_items = Gio.SimpleAction.new_stateful(
            "toggle-items", None, GLib.Variant.new_boolean(True))
        act_items.connect("change-state", self._on_action_items)
        self._view_actions.add_action(act_items)

        self._npc_location = "above"  # "above", "below", "floating"
        act_npc_loc = Gio.SimpleAction.new_stateful(
            "npc-location",
            GLib.VariantType.new("s"),
            GLib.Variant.new_string("above"))
        act_npc_loc.connect("change-state", self._on_action_npc_location)
        self._view_actions.add_action(act_npc_loc)

        self._npc_locked = False
        act_npc_lock = Gio.SimpleAction.new_stateful(
            "toggle-npc-lock", None, GLib.Variant.new_boolean(False))
        act_npc_lock.connect("change-state", self._on_action_npc_lock)
        self._view_actions.add_action(act_npc_lock)

        # Floating panel position (margin offsets from top-left of root overlay)
        self._npc_float_x = 0
        self._npc_float_y = 0

        self._loot_location = "below"  # "above", "below", "floating"
        act_loot_loc = Gio.SimpleAction.new_stateful(
            "loot-location",
            GLib.VariantType.new("s"),
            GLib.Variant.new_string("below"))
        act_loot_loc.connect("change-state", self._on_action_loot_location)
        self._view_actions.add_action(act_loot_loc)

        self._loot_locked = False
        act_loot_lock = Gio.SimpleAction.new_stateful(
            "toggle-loot-lock", None, GLib.Variant.new_boolean(False))
        act_loot_lock.connect("change-state", self._on_action_loot_lock)
        self._view_actions.add_action(act_loot_lock)

        self._loot_float_x = 0
        self._loot_float_y = 40

        # Thumbnail scale (percentage string → multiplier)
        self._SCALE_PCTS = {"50%": 0.5, "75%": 0.75, "100%": 1.0, "125%": 1.25, "150%": 1.5, "200%": 2.0}
        self._NPC_BASE_SIZE = 67
        self._LOOT_BASE_SIZE = 32
        self._npc_thumb_scale = "100%"
        self._npc_thumb_size = 67
        act_npc_scale = Gio.SimpleAction.new_stateful(
            "npc-scale", GLib.VariantType.new("s"), GLib.Variant.new_string("100%"))
        act_npc_scale.connect("change-state", self._on_action_npc_scale)
        self._view_actions.add_action(act_npc_scale)

        self._loot_thumb_scale = "100%"
        self._loot_thumb_size = 32
        act_loot_scale = Gio.SimpleAction.new_stateful(
            "loot-scale", GLib.VariantType.new("s"), GLib.Variant.new_string("100%"))
        act_loot_scale.connect("change-state", self._on_action_loot_scale)
        self._view_actions.add_action(act_loot_scale)

        # Damage text scale
        self._dmg_scale = "100%"
        self._dmg_scale_factor = 1.0
        act_dmg_scale = Gio.SimpleAction.new_stateful(
            "dmg-scale", GLib.VariantType.new("s"), GLib.Variant.new_string("100%"))
        act_dmg_scale.connect("change-state", self._on_action_dmg_scale)
        self._view_actions.add_action(act_dmg_scale)

        # Zoomed damage text scale
        self._zdmg_scale = "100%"
        self._zdmg_scale_factor = 1.0
        act_zdmg_scale = Gio.SimpleAction.new_stateful(
            "zdmg-scale", GLib.VariantType.new("s"), GLib.Variant.new_string("100%"))
        act_zdmg_scale.connect("change-state", self._on_action_zdmg_scale)
        self._view_actions.add_action(act_zdmg_scale)

        # Room description stored for Ctrl+hover overlay (no longer shown as label)
        self._current_description = ""

        # "You notice:" inline paragraph with name+thumbnail pairs (items)
        self._item_thumb_tv = Gtk.TextView()
        self._item_thumb_tv.set_editable(False)
        self._item_thumb_tv.set_cursor_visible(False)
        self._item_thumb_tv.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self._item_thumb_tv.set_halign(Gtk.Align.FILL)
        self._item_thumb_tv.set_hexpand(True)
        self._item_thumb_tv.set_margin_start(16)
        self._item_thumb_tv.set_margin_end(16)
        self._item_thumb_tv.set_margin_bottom(4)
        self._item_thumb_tv.set_visible(False)
        self._item_thumb_tv.add_css_class("inline-thumbs")
        self._item_thumb_tv.set_extra_menu(None)
        self._suppress_tv_context_menu(self._item_thumb_tv)
        self._item_thumb_scroll = self._item_thumb_tv
        self._item_thumb_widgets: list[Gtk.Widget] = []

        # "Also here:" inline paragraph with thumbnail icons (NPCs/monsters)
        self._thumb_tv = Gtk.TextView()
        self._thumb_tv.set_editable(False)
        self._thumb_tv.set_cursor_visible(False)
        self._thumb_tv.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self._thumb_tv.set_halign(Gtk.Align.FILL)
        self._thumb_tv.set_hexpand(True)
        self._thumb_tv.set_margin_start(16)
        self._thumb_tv.set_margin_end(16)
        self._thumb_tv.set_margin_top(4)
        self._thumb_tv.set_margin_bottom(4)
        self._thumb_tv.set_visible(False)
        self._thumb_tv.add_css_class("inline-thumbs")
        self._thumb_tv.set_extra_menu(None)
        self._suppress_tv_context_menu(self._thumb_tv)
        self._thumb_scroll = self._thumb_tv

        # NPC panel: monsters/NPCs + drag handle
        self._npc_panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self._npc_panel.add_css_class("npc-panel")

        self._npc_handle = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        self._npc_handle.set_size_request(-1, 10)
        self._npc_handle.add_css_class("npc-handle")
        self._npc_handle.set_cursor_from_name("grab")
        self._npc_handle.set_visible(False)
        self._npc_panel.append(self._npc_handle)
        self._npc_panel.append(self._thumb_tv)

        npc_drag = Gtk.GestureDrag()
        npc_drag.set_button(1)
        npc_drag.connect("drag-begin", self._on_npc_drag_begin)
        npc_drag.connect("drag-update", self._on_npc_drag_update)
        npc_drag.connect("drag-end", self._on_npc_drag_end)
        self._npc_panel.add_controller(npc_drag)
        self._npc_drag_start_x = 0.0
        self._npc_drag_start_y = 0.0
        self._npc_dragging = False

        # Loot panel: items + drag handle (separate from NPC panel)
        self._loot_panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self._loot_panel.add_css_class("npc-panel")

        self._loot_handle = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        self._loot_handle.set_size_request(-1, 10)
        self._loot_handle.add_css_class("npc-handle")
        self._loot_handle.set_cursor_from_name("grab")
        self._loot_handle.set_visible(False)
        self._loot_panel.append(self._loot_handle)
        self._loot_panel.append(self._item_thumb_tv)

        loot_drag = Gtk.GestureDrag()
        loot_drag.set_button(1)
        loot_drag.connect("drag-begin", self._on_loot_drag_begin)
        loot_drag.connect("drag-update", self._on_loot_drag_update)
        loot_drag.connect("drag-end", self._on_loot_drag_end)
        self._loot_panel.add_controller(loot_drag)
        self._loot_drag_start_x = 0.0
        self._loot_drag_start_y = 0.0
        self._loot_dragging = False

        # Exits box created here but appended after image below
        self._exits_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        self._exits_box.set_halign(Gtk.Align.CENTER)
        self._exits_box.set_margin_start(16)
        self._exits_box.set_margin_end(16)
        self._exits_box.set_margin_top(4)
        self._exits_box.set_margin_bottom(4)
        self._exit_widgets: list[Gtk.Widget] = []

        # Hover popup for enlarged thumbnail
        self._hover_popup = Gtk.Window()
        self._hover_popup.set_decorated(False)
        self._hover_popup.set_default_size(280, 310)
        self._hover_popup.set_resizable(False)
        self._hover_anim_timer = None
        self._hover_anim_start = 0.0
        self._hover_anim_phase = "idle"  # idle, zoom_in, float, zoom_out
        self._ctx_menu_active = False  # suppress hover-leave while menu is open
        self._hover_thumb_rect = (0, 0, 67, 67)  # screen x,y,w,h of source thumbnail
        self._hover_final_rect = (0, 0, 280, 310)  # final popup x,y,w,h
        self._hover_popup_image = Gtk.Picture()
        self._hover_popup_image.set_content_fit(Gtk.ContentFit.CONTAIN)
        self._hover_popup_image.set_size_request(256, 256)
        self._hover_popup_image.set_margin_start(12)
        self._hover_popup_image.set_margin_end(12)
        self._hover_popup_image.set_margin_top(12)
        popup_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        popup_box.add_css_class("hover-popup")
        popup_box.append(self._hover_popup_image)
        self._hover_popup_label = Gtk.Label(label="")
        self._hover_popup_label.set_margin_start(12)
        self._hover_popup_label.set_margin_end(12)
        self._hover_popup_label.set_margin_top(8)
        self._hover_popup_label.set_margin_bottom(4)
        self._hover_popup_label.add_css_class("hover-label")
        popup_box.append(self._hover_popup_label)
        # Equipment list for players (shown below name)
        self._hover_equip_label = Gtk.Label(label="")
        self._hover_equip_label.set_wrap(True)
        self._hover_equip_label.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self._hover_equip_label.set_halign(Gtk.Align.START)
        self._hover_equip_label.set_margin_start(12)
        self._hover_equip_label.set_margin_end(12)
        self._hover_equip_label.set_margin_bottom(12)
        self._hover_equip_label.set_max_width_chars(35)
        self._hover_equip_label.add_css_class("equip-label")
        popup_box.append(self._hover_equip_label)

        # Monster stats label (shown below name for monsters with gamedata)
        self._hover_stats_label = Gtk.Label(label="")
        self._hover_stats_label.set_wrap(True)
        self._hover_stats_label.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self._hover_stats_label.set_halign(Gtk.Align.START)
        self._hover_stats_label.set_margin_start(12)
        self._hover_stats_label.set_margin_end(12)
        self._hover_stats_label.set_max_width_chars(35)
        self._hover_stats_label.set_visible(False)
        self._hover_stats_label.add_css_class("stats-label")
        popup_box.append(self._hover_stats_label)

        # Monster drop table label
        self._hover_drops_label = Gtk.Label(label="")
        self._hover_drops_label.set_wrap(True)
        self._hover_drops_label.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self._hover_drops_label.set_halign(Gtk.Align.START)
        self._hover_drops_label.set_margin_start(12)
        self._hover_drops_label.set_margin_end(12)
        self._hover_drops_label.set_margin_bottom(12)
        self._hover_drops_label.set_max_width_chars(35)
        self._hover_drops_label.set_visible(False)
        self._hover_drops_label.add_css_class("drops-label")
        popup_box.append(self._hover_drops_label)

        # Wrap popup in overlay for floating damage numbers
        self._hover_overlay = Gtk.Overlay()
        self._hover_overlay.set_child(popup_box)
        self._hover_effects_da = Gtk.DrawingArea()
        self._hover_effects_da.set_can_target(False)
        self._hover_effects_da.set_hexpand(True)
        self._hover_effects_da.set_vexpand(True)
        self._hover_effects_da.set_draw_func(self._on_hover_effects_draw)
        self._hover_overlay.add_overlay(self._hover_effects_da)
        self._hover_popup.set_child(self._hover_overlay)

        # Hover state
        self._hover_damage_numbers: list[dict] = []
        self._hover_effects_ticking = False
        self._hover_entity_name = ""  # currently hovered entity name (lowercase)
        self._hover_shatter_active = False
        self._hover_img_overlay_center = None  # (x, y) in overlay coords for shatter

        # Track thumbnail widgets for cleanup
        self._thumb_widgets: list[Gtk.Widget] = []
        # Map entity display_name (lowercase) -> widget for combat effects
        self._thumb_by_name: dict[str, Gtk.Widget] = {}
        # Map entity display_name (lowercase) -> pixbuf for shatter effect
        self._thumb_pixbufs: dict[str, GdkPixbuf.Pixbuf] = {}
        # Map entity display_name (lowercase) -> HP bar DrawingArea
        self._hp_bar_widgets: dict[str, Gtk.DrawingArea] = {}
        # Map entity_key -> placeholder label for live progress updates
        self._thumb_progress_labels: dict[str, Gtk.Label] = {}
        self._item_progress_labels: dict[str, Gtk.Label] = {}
        # Ripple effect: store source pixbuf for distortion
        self._hover_src_pixbuf = None
        self._hover_src_widget = None  # the thumbnail widget being hovered

        # Combat visual effects overlay
        self._combat_effects = CombatEffects(self._root_overlay)
        # Death shatter: flag set by XP gain, checked on next thumbnail update
        self._pending_death = False

        # Image area — overlay for cross-fade (right below header bar)
        self._image_overlay = Gtk.Overlay()
        self._image_overlay.set_size_request(-1, 200)  # minimum height, scales with window
        self._image_overlay.set_vexpand(True)
        self._vbox.append(self._image_overlay)

        # Place monster/item thumbnails (above or below image based on toggle)
        self._place_thumb_widgets()

        # Exits footer (below thumbnails, centered)
        self._vbox.append(self._exits_box)

        # Background image (old, fading out)
        self._image_old = Gtk.Picture()
        self._image_old.set_content_fit(Gtk.ContentFit.COVER)
        self._image_old.set_can_shrink(True)
        self._image_old.set_opacity(1.0)
        self._image_overlay.set_child(self._image_old)

        # Foreground image (new, fading in)
        self._image_new = Gtk.Picture()
        self._image_new.set_content_fit(Gtk.ContentFit.COVER)
        self._image_new.set_can_shrink(True)
        self._image_new.set_opacity(0.0)
        self._image_overlay.add_overlay(self._image_new)

        # Loading placeholder overlay
        self._placeholder_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self._placeholder_box.set_halign(Gtk.Align.FILL)
        self._placeholder_box.set_valign(Gtk.Align.FILL)
        self._placeholder_box.set_hexpand(True)
        self._placeholder_box.set_vexpand(True)

        self._placeholder_drawing = Gtk.DrawingArea()
        self._placeholder_drawing.set_hexpand(True)
        self._placeholder_drawing.set_vexpand(True)
        self._placeholder_drawing.set_draw_func(self._draw_placeholder)
        self._placeholder_box.append(self._placeholder_drawing)

        self._image_overlay.add_overlay(self._placeholder_box)
        self._placeholder_box.set_visible(True)
        self._placeholder_hue = 0.0
        self._placeholder_timer: Optional[int] = GLib.timeout_add(50, self._placeholder_tick)

        # Description overlay (Ctrl+hover on image area)
        self._desc_overlay_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self._desc_overlay_box.set_halign(Gtk.Align.FILL)
        self._desc_overlay_box.set_valign(Gtk.Align.FILL)
        self._desc_overlay_box.set_visible(False)
        self._desc_overlay_box.add_css_class("desc-overlay")

        self._desc_overlay_label = Gtk.Label()
        self._desc_overlay_label.set_wrap(True)
        self._desc_overlay_label.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self._desc_overlay_label.set_max_width_chars(65)
        self._desc_overlay_label.set_halign(Gtk.Align.CENTER)
        self._desc_overlay_label.set_valign(Gtk.Align.CENTER)
        self._desc_overlay_label.set_margin_start(24)
        self._desc_overlay_label.set_margin_end(24)
        self._desc_overlay_label.set_margin_top(24)
        self._desc_overlay_label.set_margin_bottom(24)
        self._desc_overlay_label.add_css_class("desc-overlay-text")
        self._desc_overlay_box.append(self._desc_overlay_label)
        self._image_overlay.add_overlay(self._desc_overlay_box)

        # Ctrl+hover tracking for description overlay
        self._ctrl_held = False
        self._hovering_image = False

        key_ctrl = Gtk.EventControllerKey()
        key_ctrl.connect("key-pressed", self._on_key_pressed)
        key_ctrl.connect("key-released", self._on_key_released)
        self.add_controller(key_ctrl)

        img_hover = Gtk.EventControllerMotion()
        img_hover.connect("enter", self._on_image_hover_enter)
        img_hover.connect("leave", self._on_image_hover_leave)
        self._image_overlay.add_controller(img_hover)

        # Status bar (single line)
        self._status_label = Gtk.Label(label="Model loading...")
        self._status_label.set_halign(Gtk.Align.START)
        self._status_label.set_margin_start(16)
        self._status_label.set_margin_top(4)
        self._status_label.set_margin_bottom(0)
        self._status_label.add_css_class("dim-label")
        self._vbox.append(self._status_label)

        # Mini console log (scrollable, shows generation activity)
        self._console_scroll = Gtk.ScrolledWindow()
        self._console_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self._console_scroll.set_min_content_height(60)
        self._console_scroll.set_max_content_height(80)
        self._console_text = Gtk.TextView()
        self._console_text.set_editable(False)
        self._console_text.set_cursor_visible(False)
        self._console_text.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self._console_text.set_monospace(True)
        self._console_text.add_css_class("dim-label")
        self._console_text.set_margin_start(8)
        self._console_text.set_margin_end(8)
        self._console_buffer = self._console_text.get_buffer()
        self._console_scroll.set_child(self._console_text)
        self._vbox.append(self._console_scroll)
        self._console_max_lines = 50
        # Progress bar tracking: bar_id -> (start_mark, end_mark)
        self._progress_bars: dict[str, tuple] = {}

        # Right-click on room image to regenerate
        room_drag = self._hold_menu.create_gesture(self._image_overlay)
        room_drag.connect("drag-begin", self._on_room_image_right_drag)

        # Callbacks (set by main app)
        self._on_regenerate_room: Optional[Callable[[], None]] = None
        self._on_regenerate_entity: Optional[Callable[[str], None]] = None
        self._inject_fn: Optional[Callable[[str], None]] = None

        # Track current room name for context menu
        self._current_room_name = ""

        # Action group for context menu actions (Gtk.Window doesn't implement Gio.ActionMap)
        self._action_group = Gio.SimpleActionGroup()
        self.insert_action_group("ctx", self._action_group)

        # Entity DB reference for player equipment lookups
        self._entity_db = None
        # Game data reference for monster stats/drops
        self._gamedata = None
        # HP tracker reference
        self._hp_tracker = None

        # Save references for thumbnails (set by update_thumbnails/update_item_thumbnails)
        self._get_thumb_fn: Optional[Callable] = None

        # Cross-fade state
        self._fade_step = 0
        self._fade_timer: Optional[int] = None
        self._has_first_image = False
        self._last_image_bytes: Optional[bytes] = None

        # Effect toggles
        self._scanlines_enabled = False
        self._warp_enabled = True

        # 3D depth parallax state
        self._3d_enabled = False
        self._3d_renderer = None       # VulkanDepthRenderer
        self._3d_params = None         # DepthRenderParams
        self._3d_timer: Optional[int] = None
        self._3d_start_time = 0.0
        self._3d_scene_ready = False   # True when depth scene loaded for current room
        self._on_3d_toggled_cb: Optional[Callable[[bool], None]] = None

        # Apply CSS for nicer fonts
        css = Gtk.CssProvider()
        css.load_from_string("""
            window {
                background-color: #1a1a2e;
            }
            label {
                color: #e0e0e0;
                font-family: 'Serif', 'DejaVu Serif', 'Liberation Serif', serif;
            }
            .dim-label {
                color: #888;
                font-family: 'Sans', sans-serif;
                font-size: 11px;
            }
            .hover-popup {
                background-color: #0d0d1a;
                border: 2px solid #444;
                border-radius: 10px;
                box-shadow: 0 8px 24px rgba(0, 0, 0, 0.8);
            }
            .hover-label {
                color: #ffffff;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 16px;
                font-weight: bold;
                text-shadow: 1px 1px 3px rgba(0, 0, 0, 0.9);
            }
            .equip-label {
                color: #b0b0b0;
                font-family: 'Sans', 'DejaVu Sans', sans-serif;
                font-size: 11px;
            }
            .npc-panel-floating {
                background-color: rgba(20, 20, 40, 0.85);
                border: 1px solid #555;
                border-radius: 6px;
            }
            .npc-handle {
                background-color: rgba(100, 100, 130, 0.5);
                border-radius: 4px 4px 0 0;
                min-height: 10px;
            }
            .npc-handle:hover {
                background-color: rgba(130, 130, 160, 0.7);
            }
            .desc-overlay {
                background-color: rgba(10, 10, 20, 0.85);
            }
            .desc-overlay-text {
                color: #e8e8e8;
                font-family: 'Serif', 'DejaVu Serif', serif;
                font-size: 14px;
            }
            flowboxchild {
                padding: 0;
                margin: 0;
                min-height: 0;
                min-width: 0;
            }
            .inline-thumbs {
                background-color: transparent;
                font-family: 'Serif', 'DejaVu Serif', serif;
                font-size: 11px;
            }
            .inline-thumbs text {
                background-color: transparent;
            }
        """)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            css,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

    def update_room_text(self, name: str, description: str,
                         monsters: list[str], exits: list[str],
                         items: list[str] = None) -> None:
        """Update the text portion of the display. Thread-safe via GLib.idle_add."""
        self._current_room_name = name
        def _do():
            self._room_label.set_markup(
                f"<span size='large' weight='bold'>{GLib.markup_escape_text(name)}</span>"
            )
            self._current_description = description

            # Build clickable exit labels
            for w in self._exit_widgets:
                self._exits_box.remove(w)
            self._exit_widgets.clear()

            if exits:
                prefix = Gtk.Label()
                prefix.set_markup("<span color='#81b29a' weight='bold'>Exits:</span>")
                self._exits_box.append(prefix)
                self._exit_widgets.append(prefix)

                for i, direction in enumerate(exits):
                    text = direction
                    if i < len(exits) - 1:
                        text += ","
                    lbl = Gtk.Label()
                    lbl.set_markup(
                        f"<span color='#a7c4a0'>{GLib.markup_escape_text(text)}</span>"
                    )
                    # Left-click to go that direction
                    lclick = Gtk.GestureClick()
                    lclick.set_button(1)
                    lclick.connect("pressed", self._on_exit_left_click, direction)
                    lbl.add_controller(lclick)
                    lbl.set_cursor_from_name("pointer")
                    # Right-click for "Go <direction>"
                    exit_drag = self._hold_menu.create_gesture(lbl)
                    exit_drag.connect("drag-begin", self._on_exit_right_drag, direction)
                    self._exits_box.append(lbl)
                    self._exit_widgets.append(lbl)

            # Show placeholder if we don't already have an image displayed
            if not self._has_first_image:
                self._show_placeholder()
            return False

        GLib.idle_add(_do)

    def show_generating(self) -> None:
        """Show the generating placeholder. Called when no cached image exists."""
        GLib.idle_add(self._show_placeholder)

    def set_image(self, png_bytes: bytes) -> None:
        """Set a new room image with cross-fade transition. Thread-safe."""
        # Store the raw bytes so 3D mode can resume after toggle
        self._last_image_bytes = png_bytes
        def _do():
            # In 3D mode, never show 2D images — just store bytes
            if self._3d_enabled:
                self._has_first_image = True
                if self._3d_scene_ready:
                    self._hide_placeholder()
                return False

            try:
                loader = GdkPixbuf.PixbufLoader()
                loader.write(png_bytes)
                loader.close()
                pixbuf = loader.get_pixbuf()
                if self._scanlines_enabled:
                    pixbuf = self._apply_scanlines(pixbuf, self._scanline_thickness)
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
            except Exception as e:
                logger.error(f"Failed to load image: {e}")
                return False

            self._hide_placeholder()

            if not self._has_first_image:
                # First image — just show it directly
                self._image_old.set_paintable(texture)
                self._image_old.set_opacity(1.0)
                self._image_new.set_opacity(0.0)
                self._has_first_image = True
                self._status_label.set_text("Ready")
                return False

            # Cross-fade: move current new to old, set new texture
            # Cancel any ongoing fade
            if self._fade_timer is not None:
                GLib.source_remove(self._fade_timer)
                self._fade_timer = None

            # Old gets the current foreground image
            old_paintable = self._image_new.get_paintable()
            if old_paintable:
                self._image_old.set_paintable(old_paintable)
            self._image_old.set_opacity(1.0)

            # New gets the fresh image
            self._image_new.set_paintable(texture)
            self._image_new.set_opacity(0.0)

            # Start cross-fade
            self._fade_step = 0
            interval = CROSSFADE_DURATION_MS // CROSSFADE_STEPS
            self._fade_timer = GLib.timeout_add(interval, self._fade_tick)
            self._status_label.set_text("Ready")
            return False

        GLib.idle_add(_do)

    def _fade_tick(self) -> bool:
        """One step of the cross-fade animation."""
        self._fade_step += 1
        progress = self._fade_step / CROSSFADE_STEPS

        # Ease in-out
        if progress < 0.5:
            alpha = 2 * progress * progress
        else:
            alpha = 1 - 2 * (1 - progress) * (1 - progress)

        self._image_new.set_opacity(alpha)
        self._image_old.set_opacity(1.0 - alpha)

        if self._fade_step >= CROSSFADE_STEPS:
            # Fade complete — swap
            self._image_new.set_opacity(1.0)
            self._image_old.set_opacity(0.0)
            self._fade_timer = None
            return False  # stop timer

        return True  # continue

    def _draw_placeholder(self, area, cr, width, height):
        """Draw the hue-shifting placeholder background with text."""
        # Hue-shifting dark gradient
        hue = self._placeholder_hue
        r, g, b = colorsys.hsv_to_rgb(hue, 0.4, 0.15)
        r2, g2, b2 = colorsys.hsv_to_rgb((hue + 0.3) % 1.0, 0.3, 0.1)

        # Vertical gradient
        import cairo
        pat = cairo.LinearGradient(0, 0, 0, height)
        pat.add_color_stop_rgb(0, r, g, b)
        pat.add_color_stop_rgb(1, r2, g2, b2)
        cr.set_source(pat)
        cr.rectangle(0, 0, width, height)
        cr.fill()

        # Pulsing text
        pulse = 0.6 + 0.4 * math.sin(_time.monotonic() * 2.5)
        tr, tg, tb = colorsys.hsv_to_rgb((hue + 0.5) % 1.0, 0.3, 0.7 * pulse + 0.3)
        cr.set_source_rgb(tr, tg, tb)
        cr.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(24)
        if self._3d_enabled and not self._3d_scene_ready:
            text = "GENERATING 3D SCENE..."
        else:
            text = "GENERATING ROOM IMAGE..."
        extents = cr.text_extents(text)
        cr.move_to((width - extents.width) / 2, (height + extents.height) / 2)
        cr.show_text(text)

        # Subtitle
        cr.set_font_size(14)
        sub = "Please wait"
        extents2 = cr.text_extents(sub)
        cr.set_source_rgba(tr, tg, tb, 0.6)
        cr.move_to((width - extents2.width) / 2, (height + extents.height) / 2 + 30)
        cr.show_text(sub)

    def _placeholder_tick(self) -> bool:
        """Advance the placeholder hue animation."""
        if not self._placeholder_box.get_visible():
            return True  # keep timer but skip drawing
        self._placeholder_hue = (self._placeholder_hue + 0.005) % 1.0
        self._placeholder_drawing.queue_draw()
        return True

    def _show_placeholder(self) -> None:
        """Show the loading placeholder over the image area."""
        self._placeholder_box.set_visible(True)

    def _hide_placeholder(self) -> None:
        """Hide the loading placeholder."""
        self._placeholder_box.set_visible(False)

    def update_thumbnails(self, entities: list[dict], get_thumb_fn) -> None:
        """Update the thumbnail row with entity portraits.
        entities: list of {name, display_name, thumb_key, has_thumb, has_base_thumb}
        get_thumb_fn: callable(key) -> Optional[bytes]
        Thread-safe via GLib.idle_add.
        """
        self._last_npc_update_args = (entities, get_thumb_fn)
        self._get_thumb_fn = get_thumb_fn
        def _do():
            # Shatter any monsters that disappeared since last update (death)
            if self._pending_death and self._thumb_by_name:
                new_names = {e["display_name"].lower() for e in entities}
                for old_name, old_widget in self._thumb_by_name.items():
                    if old_name not in new_names:
                        # Notify HP tracker of death
                        if self._hp_tracker:
                            self._hp_tracker.on_monster_death(old_name)
                        # If hovering this monster, do the big shatter instead of thumbnail
                        if old_name == self._hover_entity_name and self._hover_src_pixbuf:
                            self._start_hover_shatter()
                            old_widget.set_opacity(0.0)
                        else:
                            pixbuf = self._thumb_pixbufs.get(old_name)
                            if pixbuf and old_widget.get_mapped():
                                self._combat_effects.spawn_shatter(old_widget, pixbuf)
                                old_widget.set_opacity(0.0)
                self._combat_effects.clear_target()
            self._pending_death = False

            # Clear old thumbnails
            self._thumb_progress_labels.clear()
            self._thumb_widgets.clear()
            self._thumb_by_name.clear()
            self._thumb_pixbufs.clear()
            self._hp_bar_widgets.clear()
            buf = self._thumb_tv.get_buffer()
            buf.set_text("")

            if not entities:
                self._thumb_scroll.set_visible(False)
                self._npc_panel.set_visible(False)
                return False

            self._npc_panel.set_visible(True)
            self._thumb_scroll.set_visible(self._show_monsters)

            # Create text tags for this buffer
            tag_table = buf.get_tag_table()
            if not tag_table.lookup("npc_prefix"):
                buf.create_tag("npc_prefix", foreground="#e8a87c",
                               weight=Pango.Weight.BOLD,
                               scale=1.3)
            if not tag_table.lookup("npc_name"):
                buf.create_tag("npc_name", foreground="#cf6679",
                               scale=1.3)
            if not tag_table.lookup("npc_sep"):
                buf.create_tag("npc_sep", foreground="#888888",
                               scale=1.3)

            # "Also here: " prefix
            end = buf.get_end_iter()
            buf.insert_with_tags_by_name(end, "Also here: ", "npc_prefix")

            for i, ent in enumerate(entities):
                thumb_key = ent["thumb_key"]
                display_name = ent["display_name"]

                # Try to get thumbnail: prefixed version first, then base
                png = None
                if ent.get("has_thumb"):
                    png = get_thumb_fn(thumb_key)
                if png is None and ent.get("has_base_thumb"):
                    png = get_thumb_fn(ent["base_name"])

                # Build thumbnail widget
                sz = self._npc_thumb_size
                thumb_widget = self._make_thumb_widget(png, display_name, sz)
                # Track by name for combat effects
                name_lower = display_name.lower()
                self._thumb_by_name[name_lower] = thumb_widget
                if png:
                    try:
                        loader = GdkPixbuf.PixbufLoader()
                        loader.write(png)
                        loader.close()
                        pb = loader.get_pixbuf()
                        self._thumb_pixbufs[name_lower] = pb.scale_simple(
                            sz, sz, GdkPixbuf.InterpType.BILINEAR)
                    except Exception:
                        pass
                if not png:
                    pct_lbl = self._get_spinner_label(thumb_widget)
                    if pct_lbl:
                        self._thumb_progress_labels[thumb_key] = pct_lbl
                        if ent.get("base_name"):
                            self._thumb_progress_labels[ent["base_name"]] = pct_lbl

                # Hover controller for enlarged popup (shows name)
                hover = Gtk.EventControllerMotion()
                hover.set_propagation_limit(Gtk.PropagationLimit.NONE)
                hover.connect("enter", self._on_thumb_enter, display_name, thumb_key,
                              ent.get("base_name", ""), get_thumb_fn)
                hover.connect("leave", self._on_thumb_leave)
                thumb_widget.add_controller(hover)

                # Right-click hold menu
                drag = self._hold_menu.create_gesture(thumb_widget)
                drag.connect("drag-begin", self._on_thumb_right_drag,
                             display_name, thumb_key, ent.get("base_name", ""))

                # Insert thumbnail as child anchor in the textview
                end = buf.get_end_iter()
                anchor = buf.create_child_anchor(end)
                self._thumb_tv.add_child_at_anchor(thumb_widget, anchor)
                self._thumb_widgets.append(thumb_widget)

                # Comma separator (just icons with ", " between)
                if i < len(entities) - 1:
                    end = buf.get_end_iter()
                    buf.insert_with_tags_by_name(end, ", ", "npc_sep")

            # Show full green HP bars for all monsters on room entry
            for key, bar in self._hp_bar_widgets.items():
                bar._hp_fraction = 1.0
                bar.set_visible(True)
                bar.queue_draw()

            # Force panel to re-measure after child anchors are laid out
            def _force_resize():
                self._thumb_tv.queue_resize()
                self._npc_panel.queue_resize()
                return False
            GLib.timeout_add(50, _force_resize)

            return False

        GLib.idle_add(_do)

    def update_thumb_progress(self, entity_key: str, fraction: float) -> None:
        """Update the progress % on a generating thumbnail. Thread-safe."""
        def _do():
            pct_text = f"{int(fraction * 100)}%"
            lbl = self._thumb_progress_labels.get(entity_key)
            if lbl:
                lbl.set_text(pct_text)
            lbl2 = self._item_progress_labels.get(entity_key)
            if lbl2:
                lbl2.set_text(pct_text)
            return False
        GLib.idle_add(_do)

    def update_item_thumbnails(self, items: list[dict], get_thumb_fn) -> None:
        """Update the item row: name [thumbnail] pairs inline as paragraph."""
        self._last_loot_update_args = (items, get_thumb_fn)
        def _do():
            self._item_progress_labels.clear()
            self._item_thumb_widgets.clear()
            buf = self._item_thumb_tv.get_buffer()
            buf.set_text("")

            if not items:
                self._item_thumb_scroll.set_visible(False)
                self._loot_panel.set_visible(False)
                return False

            self._loot_panel.set_visible(True)
            self._item_thumb_scroll.set_visible(self._show_items)

            # Create text tags for this buffer
            tag_table = buf.get_tag_table()
            if not tag_table.lookup("item_prefix"):
                buf.create_tag("item_prefix", foreground="#b8a9c9",
                               weight=Pango.Weight.BOLD)
            if not tag_table.lookup("item_name"):
                buf.create_tag("item_name", foreground="#d4c5e2")
            if not tag_table.lookup("item_sep"):
                buf.create_tag("item_sep", foreground="#888888")

            # "You notice: " prefix
            end = buf.get_end_iter()
            buf.insert_with_tags_by_name(end, "You notice: ", "item_prefix")

            for i, item in enumerate(items):
                thumb_key = item["thumb_key"]
                display_name = item["display_name"]
                qty = item.get("quantity", 1)

                # Insert name text (with quantity prefix)
                qty_prefix = f"{qty} " if qty > 1 else ""
                end = buf.get_end_iter()
                buf.insert_with_tags_by_name(end, qty_prefix + display_name, "item_name")

                # Build thumbnail widget
                png = None
                if item.get("has_thumb"):
                    png = get_thumb_fn(thumb_key)

                loot_sz = self._loot_thumb_size
                thumb_widget = self._make_thumb_widget(png, display_name, loot_sz)
                if not png:
                    pct_lbl = self._get_spinner_label(thumb_widget)
                    if pct_lbl:
                        self._item_progress_labels[thumb_key] = pct_lbl

                # Wrap in overlay for quantity badge
                if qty > 1:
                    overlay = Gtk.Overlay()
                    overlay.set_child(thumb_widget)
                    badge = Gtk.Label()
                    badge.set_halign(Gtk.Align.START)
                    badge.set_valign(Gtk.Align.START)
                    badge.set_markup(
                        f"<span size='7000' weight='bold' color='#fff' "
                        f"background='#c04040'> {qty} </span>"
                    )
                    overlay.add_overlay(badge)
                    final_widget = overlay
                else:
                    final_widget = thumb_widget

                # Hover for enlarged view
                hover = Gtk.EventControllerMotion()
                hover.set_propagation_limit(Gtk.PropagationLimit.NONE)
                hover.connect("enter", self._on_thumb_enter, display_name, thumb_key,
                              "", get_thumb_fn)
                hover.connect("leave", self._on_thumb_leave)
                final_widget.add_controller(hover)

                # Double-click to pick up item (sends "get <item>")
                dblclick = Gtk.GestureClick()
                dblclick.set_button(1)  # left button
                dblclick.connect("released", self._on_item_double_click, display_name)
                final_widget.add_controller(dblclick)

                # Right-click hold menu
                drag = self._hold_menu.create_gesture(final_widget)
                drag.connect("drag-begin", self._on_item_right_drag, display_name, thumb_key)

                # Insert thumbnail as child anchor
                end = buf.get_end_iter()
                anchor = buf.create_child_anchor(end)
                self._item_thumb_tv.add_child_at_anchor(final_widget, anchor)
                self._item_thumb_widgets.append(final_widget)

                # Comma separator
                if i < len(items) - 1:
                    end = buf.get_end_iter()
                    buf.insert_with_tags_by_name(end, ", ", "item_sep")

            return False

        GLib.idle_add(_do)

    @staticmethod
    def _suppress_tv_context_menu(tv: Gtk.TextView) -> None:
        """Remove all default event controllers from a TextView (context menu, selection, etc)."""
        # Collect controllers first to avoid mutating while iterating
        controllers = []
        ctrl = tv.observe_controllers()
        for i in range(ctrl.get_n_items()):
            controllers.append(ctrl.get_item(i))
        for c in controllers:
            tv.remove_controller(c)
        # Set cursor to default arrow instead of text cursor
        tv.set_cursor_from_name("default")

    def _make_thumb_widget(self, png: bytes | None, display_name: str, size: int) -> Gtk.Widget:
        """Create a square thumbnail widget (Frame with image or spinner), with HP bar overlay."""
        frame = Gtk.Frame()
        frame.set_size_request(size, size)
        if png:
            try:
                loader = GdkPixbuf.PixbufLoader()
                loader.write(png)
                loader.close()
                pixbuf = loader.get_pixbuf()
                pixbuf = pixbuf.scale_simple(size, size, GdkPixbuf.InterpType.BILINEAR)
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                pic = Gtk.Picture()
                pic.set_paintable(texture)
                pic.set_size_request(size, size)
                pic.set_content_fit(Gtk.ContentFit.COVER)
                frame.set_child(pic)
            except Exception as e:
                logger.error(f"Failed to load thumb for {display_name}: {e}")
        else:
            placeholder = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            placeholder.set_size_request(size, size)
            placeholder.set_valign(Gtk.Align.CENTER)
            placeholder.set_halign(Gtk.Align.CENTER)
            spinner = Gtk.Spinner()
            spinner.set_size_request(min(size, 20), min(size, 20))
            spinner.start()
            placeholder.append(spinner)
            pct_lbl = Gtk.Label(label="...")
            pct_lbl.add_css_class("dim-label")
            placeholder.append(pct_lbl)
            frame.set_child(placeholder)

        # Wrap in overlay for HP bar
        overlay = Gtk.Overlay()
        overlay.set_child(frame)
        overlay.set_size_request(size, size)
        overlay.set_tooltip_text(display_name)

        # Add HP bar drawing area at bottom
        hp_bar = Gtk.DrawingArea()
        hp_bar.set_size_request(size, 8)
        hp_bar.set_valign(Gtk.Align.END)
        hp_bar.set_can_target(False)
        hp_bar._hp_fraction = None  # will be set when we know HP
        hp_bar.set_draw_func(self._draw_hp_bar, None)
        hp_bar.set_visible(False)
        overlay.add_overlay(hp_bar)

        # Store reference for updates
        name_key = display_name.strip().lower()
        self._hp_bar_widgets[name_key] = hp_bar

        return overlay

    @staticmethod
    def _get_spinner_label(widget: Gtk.Widget) -> Gtk.Label | None:
        """Extract the progress label from a spinner placeholder widget."""
        # If wrapped in overlay, unwrap to get the frame
        if isinstance(widget, Gtk.Overlay):
            widget = widget.get_child()
        if not isinstance(widget, Gtk.Frame):
            return None
        child = widget.get_child()
        if isinstance(child, Gtk.Box):
            c = child.get_last_child()
            if isinstance(c, Gtk.Label):
                return c
        return None

    def _on_thumb_enter(self, controller, x, y, display_name, thumb_key,
                        base_name, get_thumb_fn):
        """Show enlarged thumbnail popup on hover with zoom-in animation."""
        self._hover_entity_name = display_name.lower()
        self._hover_src_widget = controller.get_widget()
        # Try prefixed, then base
        png = get_thumb_fn(thumb_key)
        if png is None and base_name:
            png = get_thumb_fn(base_name)

        self._hover_src_pixbuf = None
        if png:
            try:
                loader = GdkPixbuf.PixbufLoader()
                loader.write(png)
                loader.close()
                pixbuf = loader.get_pixbuf()
                pixbuf = pixbuf.scale_simple(256, 256, GdkPixbuf.InterpType.BILINEAR)
                self._hover_src_pixbuf = pixbuf
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                self._hover_popup_image.set_paintable(texture)
            except Exception:
                self._hover_popup_image.set_paintable(None)
        else:
            self._hover_popup_image.set_paintable(None)

        self._hover_popup_label.set_text(display_name)

        # Show equipment list if this is a known player
        equip_text = ""
        if self._entity_db and self._entity_db.is_known_player(display_name):
            equipment = self._entity_db.get_player_equipment(display_name)
            if equipment:
                equip_text = "\n".join(equipment)
        self._hover_equip_label.set_text(equip_text)
        self._hover_equip_label.set_visible(bool(equip_text))

        # Show monster stats and drops from gamedata
        stats_text = ""
        drops_text = ""
        if self._gamedata and not (self._entity_db and self._entity_db.is_known_player(display_name)):
            stats = self._gamedata.get_monster_stats(base_name or display_name)
            if stats:
                hp_line = f"HP: {stats['hp']}"
                # Show current estimated HP if tracked
                if self._hp_tracker:
                    frac = self._hp_tracker.get_hp_fraction(display_name)
                    if frac is not None:
                        est_hp = int(frac * stats['hp'])
                        hp_line = f"HP: ~{est_hp}/{stats['hp']}"
                lines = [
                    hp_line,
                    f"EXP: {stats['exp']:.0f}" if stats['exp'] else None,
                    f"AC: {stats['ac']}  DR: {stats['dr']}  MR: {stats['mr']}",
                    f"Dodge: {stats['dodge']}" if stats['dodge'] else None,
                    f"Regen: {stats['regen']}/tick" if stats['regen'] else None,
                    f"Avg Dmg: {stats['avg_dmg']:.0f}" if stats['avg_dmg'] else None,
                    "Undead" if stats['undead'] else None,
                ]
                stats_text = "\n".join(l for l in lines if l)

            drops = self._gamedata.get_monster_drops(base_name or display_name)
            if drops:
                drop_lines = [f"  {d['name']} ({d['chance']}%)" for d in drops]
                drops_text = "Drops:\n" + "\n".join(drop_lines)

        self._hover_stats_label.set_markup(
            f"<span font_desc='monospace 8'>{stats_text}</span>" if stats_text else "")
        self._hover_stats_label.set_visible(bool(stats_text))
        self._hover_drops_label.set_markup(
            f"<span font_desc='monospace 8'>{drops_text}</span>" if drops_text else "")
        self._hover_drops_label.set_visible(bool(drops_text))

        # Final popup size — grow to fit content
        final_h = 310
        if equip_text:
            final_h = 450
        if stats_text:
            final_h += 14 * stats_text.count("\n") + 30
        if drops_text:
            final_h += 14 * drops_text.count("\n") + 30
        final_w = 280

        # Get thumbnail screen position for zoom origin
        widget = controller.get_widget()
        native = widget.get_native()
        thumb_sx, thumb_sy = 0, 0
        thumb_w, thumb_h = 67, 67
        popup_x, popup_y = 0, 0
        if native:
            bounds = widget.compute_bounds(native)
            if bounds[0]:
                rect = bounds[1]
                surface = native.get_surface()
                win_x, win_y = (0, 0)
                if surface and hasattr(surface, 'get_position'):
                    win_x, win_y = surface.get_position()
                thumb_sx = int(win_x + rect.get_x())
                thumb_sy = int(win_y + rect.get_y())
                thumb_w = int(rect.get_width())
                thumb_h = int(rect.get_height())
                popup_x = thumb_sx - (final_w - thumb_w) // 2
                popup_y = thumb_sy - final_h - 10
                if popup_y < 0:
                    popup_y = 0

        self._hover_thumb_rect = (thumb_sx, thumb_sy, thumb_w, thumb_h)
        self._hover_final_rect = (popup_x, popup_y, final_w, final_h)

        # Compute popup image center in overlay coords (for combat effects)
        ok, pt = widget.compute_point(self._root_overlay, Graphene.Point().init(0, 0))
        if ok:
            tox = pt.x  # thumbnail left in overlay coords
            toy = pt.y  # thumbnail top in overlay coords
            # The popup image (256x256) is centered over the overlay.
            # Place effects at the center of the overlay for maximum visibility.
            ow = self._root_overlay.get_width()
            oh = self._root_overlay.get_height()
            img_center_x = ow / 2
            img_center_y = oh / 2 - 40  # slightly above center
            self._hover_img_overlay_center = (img_center_x, img_center_y)
            logger.debug(f"Hover overlay center: ({img_center_x:.0f}, {img_center_y:.0f}), "
                        f"thumb at ({tox:.0f}, {toy:.0f}), overlay {ow}x{oh}")
        else:
            self._hover_img_overlay_center = None
            logger.debug("compute_point failed for hover widget")

        # Start zoomed-in from thumbnail: tiny size, low opacity
        self._hover_popup.set_default_size(thumb_w, thumb_h)
        self._hover_popup.set_opacity(0.0)
        self._hover_popup.set_transient_for(self)
        self._hover_popup.present()

        # Start zoom-in animation
        self._hover_anim_phase = "zoom_in"
        self._hover_anim_start = _time.monotonic()
        if self._hover_anim_timer is not None:
            GLib.source_remove(self._hover_anim_timer)
        self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)

    @staticmethod
    def _ease_out_cubic(t: float) -> float:
        """Smooth ease-out curve."""
        return 1.0 - (1.0 - t) ** 3

    @staticmethod
    def _ease_in_cubic(t: float) -> float:
        """Smooth ease-in curve."""
        return t * t * t

    def _hover_anim_tick(self) -> bool:
        """Animate hover popup: zoom-in → float → zoom-out."""
        now = _time.monotonic()
        t = now - self._hover_anim_start
        tx, ty, tw, th = self._hover_thumb_rect
        fx, fy, fw, fh = self._hover_final_rect

        if self._hover_anim_phase == "zoom_in":
            duration = 0.25
            progress = min(t / duration, 1.0)
            ease = self._ease_out_cubic(progress)

            # Interpolate size and position from thumbnail to final
            cur_w = int(tw + (fw - tw) * ease)
            cur_h = int(th + (fh - th) * ease)
            cur_x = int(tx + (fx - tx) * ease)
            cur_y = int(ty + (fy - ty) * ease)

            self._hover_popup.set_default_size(max(cur_w, 1), max(cur_h, 1))
            self._hover_popup.set_opacity(ease)

            # Scale the image smoothly
            img_size = int(24 + (256 - 24) * ease)
            self._hover_popup_image.set_size_request(img_size, img_size)

            if progress >= 1.0:
                self._hover_anim_phase = "float"
                self._hover_anim_start = now
            return True

        elif self._hover_anim_phase == "float":
            # Apply liquid ripple distortion to the image
            # Freeze ripple while context menu is open to prevent
            # set_paintable() redraws from dismissing the popover
            if self._hover_src_pixbuf is not None and not self._ctx_menu_active and self._warp_enabled:
                self._apply_ripple(t)
            return True

        elif self._hover_anim_phase == "shatter":
            # Keep popup visible at full size — shatter is handled by _hover_effects_tick
            return True

        elif self._hover_anim_phase == "zoom_out":
            duration = 0.18
            progress = min(t / duration, 1.0)
            ease = self._ease_in_cubic(progress)

            # Interpolate from final back to thumbnail
            cur_w = int(fw + (tw - fw) * ease)
            cur_h = int(fh + (th - fh) * ease)
            cur_x = int(fx + (tx - fx) * ease)
            cur_y = int(fy + (ty - fy) * ease)

            self._hover_popup.set_default_size(max(cur_w, 1), max(cur_h, 1))
            self._hover_popup.set_opacity(1.0 - ease)

            img_size = int(256 + (24 - 256) * ease)
            self._hover_popup_image.set_size_request(max(img_size, 1), max(img_size, 1))

            if progress >= 1.0:
                self._hover_popup.set_visible(False)
                self._hover_anim_phase = "idle"
                self._hover_anim_timer = None
                # Reset to defaults
                self._hover_popup_image.set_size_request(256, 256)
                self._hover_popup_image.set_margin_top(12)
                self._hover_popup.set_default_size(280, 310)
                return False
            return True

        self._hover_anim_timer = None
        return False

    def _apply_ripple(self, t: float) -> None:
        """Apply subtle liquid ripple distortion to the hover image."""
        try:
            import numpy as np

            pb = self._hover_src_pixbuf
            w = pb.get_width()
            h = pb.get_height()
            n_channels = pb.get_n_channels()
            rowstride = pb.get_rowstride()

            # Get pixel data as numpy array
            pixels = pb.get_pixels()
            src = np.frombuffer(pixels, dtype=np.uint8).copy()
            # Reshape accounting for rowstride padding
            src_2d = np.lib.stride_tricks.as_strided(
                np.frombuffer(pixels, dtype=np.uint8),
                shape=(h, w, n_channels),
                strides=(rowstride, n_channels, 1),
            )

            # Build coordinate grids (normalized 0-1)
            ys = np.linspace(0, 1, h, dtype=np.float32)
            xs = np.linspace(0, 1, w, dtype=np.float32)
            xg, yg = np.meshgrid(xs, ys)

            # Subtle multi-source ripple (like liquid surface)
            intensity = 2.5  # pixels of max displacement
            speed = 1.0

            # Two overlapping wave sources for organic look
            dx = np.zeros_like(xg)
            dy = np.zeros_like(yg)

            for i, (cx, cy, freq, phase) in enumerate([
                (0.3, 0.4, 8.0, 0.0),
                (0.7, 0.6, 10.0, 1.5),
                (0.5, 0.2, 6.0, 3.0),
            ]):
                dist = np.sqrt((xg - cx) ** 2 + (yg - cy) ** 2)
                wave = np.sin(dist * freq * 2 * np.pi - t * speed * 3.0 + phase)
                decay = np.exp(-dist * 3.0)
                wave *= decay
                # Displacement direction: radial from source
                ddx = np.where(dist > 0.001, (xg - cx) / dist, 0)
                ddy = np.where(dist > 0.001, (yg - cy) / dist, 0)
                dx += ddx * wave * intensity
                dy += ddy * wave * intensity

            # Convert displacement to pixel coordinates
            map_x = np.clip((xg * w + dx).astype(np.int32), 0, w - 1)
            map_y = np.clip((yg * h + dy).astype(np.int32), 0, h - 1)

            # Remap pixels
            dst = src_2d[map_y, map_x]

            # Create new pixbuf from distorted data
            dst_bytes = dst.tobytes()
            new_pb = GdkPixbuf.Pixbuf.new_from_data(
                dst_bytes, GdkPixbuf.Colorspace.RGB,
                n_channels == 4, 8, w, h, w * n_channels,
            )
            # Keep reference to prevent GC of underlying bytes
            new_pb._keep_alive = dst_bytes
            texture = Gdk.Texture.new_for_pixbuf(new_pb)
            self._hover_popup_image.set_paintable(texture)

        except Exception as e:
            logger.debug(f"Ripple effect error: {e}")

    def _on_thumb_leave(self, controller, *args):
        """Start zoom-out animation back to thumbnail."""
        if self._hover_shatter_active:
            # Don't interrupt shatter — it will dismiss the popup when done
            return
        self._hover_entity_name = ""
        self._hover_damage_numbers.clear()
        if self._ctx_menu_active or self._hover_anim_phase == "menu_frozen":
            return
        if self._hover_anim_phase in ("zoom_in", "float"):
            self._hover_anim_phase = "zoom_out"
            self._hover_anim_start = _time.monotonic()
            if self._hover_anim_timer is None:
                self._hover_anim_timer = GLib.timeout_add(16, self._hover_anim_tick)
        else:
            self._hover_popup.set_visible(False)
            if self._hover_anim_timer is not None:
                GLib.source_remove(self._hover_anim_timer)
                self._hover_anim_timer = None

    def _on_hold_menu_open(self):
        """Called when hold-right-click menu opens."""
        self._ctx_menu_active = True
        # Freeze ripple animation and hide hover popup so it doesn't cover the menu
        if self._hover_anim_timer is not None:
            GLib.source_remove(self._hover_anim_timer)
            self._hover_anim_timer = None
        self._hover_anim_phase = "menu_frozen"
        self._hover_popup.set_visible(False)

    def _on_hold_menu_close(self):
        """Called when hold-right-click menu closes."""
        self._ctx_menu_active = False
        # Popup was hidden when menu opened — reset state cleanly
        self._hover_popup.set_visible(False)
        self._hover_anim_phase = None
        self._hover_popup_image.set_size_request(256, 256)
        self._hover_popup_image.set_margin_top(12)
        self._hover_popup.set_default_size(280, 310)
        self._hover_entity_key = None

    @staticmethod
    def _draw_hp_bar(da, cr, width, height, _user_data):
        """Draw a health bar on a DrawingArea. Color goes green->yellow->red."""
        frac = getattr(da, '_hp_fraction', None)
        if frac is None:
            return
        # Background (dark)
        cr.set_source_rgba(0, 0, 0, 0.6)
        cr.rectangle(0, 0, width, height)
        cr.fill()
        # Health bar color: green (1.0) -> yellow (0.5) -> red (0.0)
        if frac > 0.5:
            t = (frac - 0.5) * 2  # 1.0 at full, 0.0 at half
            r, g = 1.0 - t, 1.0
        else:
            t = frac * 2  # 1.0 at half, 0.0 at empty
            r, g = 1.0, t
        cr.set_source_rgba(r, g, 0, 0.9)
        cr.rectangle(0, 0, width * frac, height)
        cr.fill()

    def update_hp_bar(self, name: str, fraction: float) -> None:
        """Update the HP bar for a monster thumbnail."""
        key = name.strip().lower()
        bar = self._hp_bar_widgets.get(key)
        if bar is None:
            # Try without prefix
            from .gamedata import _strip_prefix
            stripped = _strip_prefix(name).strip().lower()
            bar = self._hp_bar_widgets.get(stripped)
        if bar:
            bar._hp_fraction = fraction
            bar.set_visible(True)
            bar.queue_draw()

    def update_all_hp_bars(self, fractions: dict[str, float]) -> None:
        """Update all HP bars from a dict of {name: fraction}."""
        for name, frac in fractions.items():
            self.update_hp_bar(name, frac)

    def set_entity_db(self, entity_db) -> None:
        """Set entity DB reference for player equipment lookups."""
        self._entity_db = entity_db

    def set_gamedata(self, gamedata) -> None:
        """Set game data reference for monster stats/drops."""
        self._gamedata = gamedata

    def set_hp_tracker(self, hp_tracker) -> None:
        """Set HP tracker reference for health bars."""
        self._hp_tracker = hp_tracker

    def load_view_config(self, config) -> None:
        """Load View menu settings from config and apply them."""
        self._config = config

        self._show_console = config.show_console
        self._show_monsters = config.show_monsters
        self._show_items = config.show_items
        self._scanlines_enabled = config.show_scanlines
        self._warp_enabled = config.show_warp_zoom
        self._scanline_thickness = config.scanline_thickness
        self._npc_location = config.npc_location
        self._loot_location = config.loot_location
        self._npc_locked = config.npc_locked
        self._loot_locked = config.loot_locked
        self._npc_float_x = config.npc_float_x
        self._npc_float_y = config.npc_float_y
        self._loot_float_x = config.loot_float_x
        self._loot_float_y = config.loot_float_y

        # Sync action states to match loaded config
        va = self._view_actions
        va.lookup_action("toggle-console").set_state(GLib.Variant.new_boolean(self._show_console))
        va.lookup_action("toggle-monsters").set_state(GLib.Variant.new_boolean(self._show_monsters))
        va.lookup_action("toggle-items").set_state(GLib.Variant.new_boolean(self._show_items))
        va.lookup_action("toggle-scanlines").set_state(GLib.Variant.new_boolean(self._scanlines_enabled))
        va.lookup_action("toggle-warp").set_state(GLib.Variant.new_boolean(self._warp_enabled))
        va.lookup_action("scanline-thickness").set_state(GLib.Variant.new_int32(self._scanline_thickness))
        va.lookup_action("npc-location").set_state(GLib.Variant.new_string(self._npc_location))
        va.lookup_action("loot-location").set_state(GLib.Variant.new_string(self._loot_location))
        va.lookup_action("toggle-npc-lock").set_state(GLib.Variant.new_boolean(self._npc_locked))
        va.lookup_action("toggle-loot-lock").set_state(GLib.Variant.new_boolean(self._loot_locked))

        # Scale
        self._npc_thumb_scale = getattr(config, 'npc_thumb_scale', '100%')
        mult = self._SCALE_PCTS.get(self._npc_thumb_scale, 1.0)
        self._npc_thumb_size = max(16, int(self._NPC_BASE_SIZE * mult))
        va.lookup_action("npc-scale").set_state(GLib.Variant.new_string(self._npc_thumb_scale))

        self._loot_thumb_scale = getattr(config, 'loot_thumb_scale', '100%')
        mult = self._SCALE_PCTS.get(self._loot_thumb_scale, 1.0)
        self._loot_thumb_size = max(16, int(self._LOOT_BASE_SIZE * mult))
        va.lookup_action("loot-scale").set_state(GLib.Variant.new_string(self._loot_thumb_scale))

        # Damage text scale
        self._dmg_scale = getattr(config, 'dmg_text_scale', '100%')
        self._dmg_scale_factor = self._SCALE_PCTS.get(self._dmg_scale, 1.0)
        self._combat_effects.set_font_scale(self._dmg_scale_factor)
        va.lookup_action("dmg-scale").set_state(GLib.Variant.new_string(self._dmg_scale))

        self._zdmg_scale = getattr(config, 'zdmg_text_scale', '100%')
        self._zdmg_scale_factor = self._SCALE_PCTS.get(self._zdmg_scale, 1.0)
        va.lookup_action("zdmg-scale").set_state(GLib.Variant.new_string(self._zdmg_scale))

        # Apply visibility
        self._console_scroll.set_visible(self._show_console)
        self._status_label.set_visible(self._show_console)

        # Re-place panels with loaded positions
        self._place_thumb_widgets()

    def _save_view_config(self) -> None:
        """Persist current View menu settings to config."""
        if not hasattr(self, '_config') or self._config is None:
            return
        self._config.show_console = self._show_console
        self._config.show_monsters = self._show_monsters
        self._config.show_items = self._show_items
        self._config.show_scanlines = self._scanlines_enabled
        self._config.show_warp_zoom = self._warp_enabled
        self._config.scanline_thickness = self._scanline_thickness
        self._config.npc_location = self._npc_location
        self._config.loot_location = self._loot_location
        self._config.npc_locked = self._npc_locked
        self._config.loot_locked = self._loot_locked
        self._config.npc_float_x = self._npc_float_x
        self._config.npc_float_y = self._npc_float_y
        self._config.loot_float_x = self._loot_float_x
        self._config.loot_float_y = self._loot_float_y
        self._config.npc_thumb_scale = self._npc_thumb_scale
        self._config.loot_thumb_scale = self._loot_thumb_scale
        self._config.dmg_text_scale = self._dmg_scale
        self._config.zdmg_text_scale = self._zdmg_scale
        self._config.save()

    def set_inject_fn(self, fn: Callable[[str], None]) -> None:
        """Set command injection callback."""
        self._inject_fn = fn

    def set_regenerate_room_callback(self, fn: Callable[[], None]) -> None:
        self._on_regenerate_room = fn

    def set_regenerate_entity_callback(self, fn: Callable[[str], None]) -> None:
        self._on_regenerate_entity = fn

    # --- Combat Effects ---

    def _find_thumb_by_name(self, target: str):
        """Find thumbnail widget and pixbuf by name, with fuzzy suffix matching.
        Death messages may drop prefix adjectives (e.g. 'orc rogue' vs 'fat orc rogue')."""
        key = target.lower()
        widget = self._thumb_by_name.get(key)
        if widget:
            return key, widget
        # Fuzzy: check if any known name ends with the target
        for name, w in self._thumb_by_name.items():
            if name.endswith(key) or key.endswith(name):
                return name, w
        return None, None

    def show_damage(self, target_name: str, damage: int,
                    color: tuple = (1.0, 0.15, 0.1),
                    crit: bool = False,
                    surprise: bool = False) -> None:
        """Show a floating damage number over a monster thumbnail. Thread-safe."""
        def _do():
            key, widget = self._find_thumb_by_name(target_name)
            if widget and widget.get_mapped():
                # Restore visibility if previously shattered (same mob respawned)
                if widget.get_opacity() < 0.5:
                    widget.set_opacity(1.0)
                self._combat_effects.spawn_damage(widget, damage, color,
                                                  crit=crit,
                                                  surprise=surprise)
                # Set as active combat target (glowing border)
                self._combat_effects.set_target(widget)

                # Also spawn on hover popup if this entity is being hovered
                if self._hover_entity_name and target_name.lower() == self._hover_entity_name:
                    self._spawn_hover_damage(damage, color, crit, surprise)
            return False
        GLib.idle_add(_do)

    def _spawn_hover_damage(self, damage: int, color: tuple,
                            crit: bool, surprise: bool) -> None:
        """Spawn a scaled-up floating damage number on the hover popup."""
        import random
        now = _time.monotonic()

        s = self._zdmg_scale_factor
        if surprise:
            font_size = 64 * s
            label = f"BS {damage}!"
            duration = 1.6
            r, g, b = 0.7, 0.2, 1.0
        elif crit:
            font_size = 58 * s
            label = f"CRIT {damage}!"
            duration = 1.4
            r, g, b = color
        else:
            font_size = (42 + (6 if damage >= 50 else 0)) * s
            label = str(damage)
            duration = 1.0
            r, g, b = color

        # Spawn centered in the 256x256 image area (offset by 12px margin)
        # Stagger vertically based on active count so numbers don't pile up
        active_count = len(self._hover_damage_numbers)
        x = 12 + 128 + random.randint(-60, 60) + (20 if active_count % 2 else -20)
        y = 12 + 128 + random.randint(-20, 20) - active_count * 24

        self._hover_damage_numbers.append({
            "label": label, "x": x, "y": y,
            "vx": random.uniform(-20, 20), "vy": -80 - random.uniform(0, 40),
            "r": r, "g": g, "b": b,
            "font_size": font_size, "t0": now, "duration": duration,
            "crit": crit, "surprise": surprise,
        })

        if not self._hover_effects_ticking:
            self._hover_effects_ticking = True
            GLib.timeout_add(16, self._hover_effects_tick)

    def _hover_effects_tick(self) -> bool:
        """Animate hover popup damage numbers."""
        if not self._hover_damage_numbers:
            self._hover_effects_ticking = False
            self._hover_effects_da.queue_draw()
            return False
        self._hover_effects_da.queue_draw()
        return True

    def _on_hover_effects_draw(self, da, cr, width, height) -> None:
        """Cairo draw function for hover popup floating damage numbers."""
        now = _time.monotonic()

        if not self._hover_damage_numbers:
            return

        alive = []
        for num in self._hover_damage_numbers:
            t = now - num["t0"]
            if t > num["duration"]:
                continue
            alive.append(num)

            frac = t / num["duration"]
            alpha = 1.0 - frac * frac
            x = num["x"] + num["vx"] * t
            y = num["y"] + num["vy"] * t + 40 * t * t

            cr.select_font_face("Sans", 0, 1)  # NORMAL, BOLD
            cr.set_font_size(num["font_size"])
            label = num["label"]

            if num["surprise"]:
                cr.set_source_rgba(0, 0, 0, alpha * 0.8)
                cr.move_to(x + 3, y + 3)
                cr.show_text(label)
                cr.set_source_rgba(num["r"], num["g"], num["b"], alpha)
                cr.move_to(x, y)
                cr.show_text(label)
                cr.set_source_rgba(0.9, 0.6, 1.0, alpha * 0.5)
                cr.move_to(x, y - 1)
                cr.show_text(label)
            elif num["crit"]:
                import math as _m
                pulse = 0.6 + 0.4 * _m.sin(t * 12 * _m.pi)
                cr.set_source_rgba(0, 0, 0, alpha)
                for dx, dy in [(-2, -2), (2, -2), (-2, 2), (2, 2)]:
                    cr.move_to(x + dx, y + dy)
                    cr.show_text(label)
                cr.set_source_rgba(1.0, 0.85 * pulse, 0.2, alpha)
                cr.move_to(x, y)
                cr.show_text(label)
            else:
                cr.set_source_rgba(0, 0, 0, alpha)
                for dx, dy in [(-1, -1), (1, -1), (-1, 1), (1, 1)]:
                    cr.move_to(x + dx, y + dy)
                    cr.show_text(label)
                cr.set_source_rgba(num["r"], num["g"], num["b"], alpha)
                cr.move_to(x, y)
                cr.show_text(label)

        self._hover_damage_numbers = alive

    def _start_hover_shatter(self) -> None:
        """Shatter the hover popup image — hide popup, render on main room overlay."""
        pb = self._hover_src_pixbuf
        if pb is None:
            return

        # Freeze the hover animation — don't zoom out
        self._hover_anim_phase = "shatter"
        self._hover_shatter_active = True

        # Hide popup IMMEDIATELY so shatter is visible on the room overlay
        self._hover_popup.set_visible(False)

        # Use full-res pixbuf for crisp shatter shards
        shatter_size = 256
        pb_big = pb.scale_simple(shatter_size, shatter_size, GdkPixbuf.InterpType.BILINEAR)

        # Center the shatter in the middle of the room overlay
        ow = self._root_overlay.get_width()
        oh = self._root_overlay.get_height()
        origin_x = ow / 2 - shatter_size / 2
        origin_y = oh / 2 - shatter_size / 2

        # Spawn on the main combat effects overlay
        self._combat_effects.spawn_shatter_at(
            origin_x, origin_y, shatter_size, shatter_size, pb_big,
            cols=8, rows=8, speed_range=(120, 350))

        # Schedule cleanup
        def _finish():
            self._hover_shatter_active = False
            self._hover_entity_name = ""
            self._hover_anim_phase = "idle"
            if self._hover_anim_timer is not None:
                GLib.source_remove(self._hover_anim_timer)
                self._hover_anim_timer = None
            self._hover_popup_image.set_opacity(1.0)
            self._hover_popup_image.set_size_request(256, 256)
            self._hover_popup_image.set_margin_top(12)
            self._hover_popup.set_default_size(280, 310)
            return False
        GLib.timeout_add(1800, _finish)

        if self._hover_anim_timer is not None:
            GLib.source_remove(self._hover_anim_timer)
            self._hover_anim_timer = None
        self._hover_popup_image.set_size_request(256, 256)
        self._hover_popup_image.set_margin_top(12)
        self._hover_popup.set_default_size(280, 310)

    def notify_xp_gained(self) -> None:
        """Flag that XP was gained — next thumbnail update will shatter missing monsters."""
        self._pending_death = True

    def set_combat_target(self, target_name: str) -> None:
        """Set the glowing red border on a monster by name. Thread-safe."""
        def _do():
            key, widget = self._find_thumb_by_name(target_name)
            if widget and widget.get_mapped():
                if widget.get_opacity() < 0.5:
                    widget.set_opacity(1.0)
                self._combat_effects.set_target(widget)
            return False
        GLib.idle_add(_do)

    def clear_combat_target(self) -> None:
        """Clear the glowing combat target border. Thread-safe."""
        def _do():
            self._combat_effects.clear_target()
            return False
        GLib.idle_add(_do)

    # --- Save / Export ---

    def _save_file_dialog(self, title: str, default_name: str, filter_name: str,
                          filter_pattern: str, callback: Callable[[str], None]) -> None:
        """Open a GTK4 file save dialog."""
        dialog = Gtk.FileDialog()
        dialog.set_title(title)
        dialog.set_initial_name(default_name)
        file_filter = Gtk.FileFilter()
        file_filter.set_name(filter_name)
        file_filter.add_pattern(filter_pattern)
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(file_filter)
        dialog.set_filters(filters)
        dialog.set_default_filter(file_filter)
        # Default to ~/Pictures if it exists
        pictures = Path.home() / "Pictures"
        if pictures.is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(pictures)))
        dialog.save(self, None, lambda d, res: self._on_save_dialog_done(d, res, callback))

    def _on_save_dialog_done(self, dialog, result, callback):
        try:
            gfile = dialog.save_finish(result)
            if gfile:
                callback(gfile.get_path())
        except GLib.Error:
            pass  # user cancelled

    def _save_png(self, png_bytes: bytes, path: str) -> None:
        """Write PNG bytes to disk."""
        try:
            if not path.lower().endswith('.png'):
                path += '.png'
            Path(path).write_bytes(png_bytes)
            self._status_label.set_text(f"Saved: {Path(path).name}")
        except Exception as e:
            logger.error(f"Failed to save PNG: {e}")
            self._status_label.set_text(f"Save failed: {e}")

    def _save_room_as_png(self) -> None:
        """Save current room image as PNG."""
        if not self._last_image_bytes:
            self._status_label.set_text("No room image to save")
            return
        room_name = self._current_room_name or "room"
        safe_name = "".join(c if c.isalnum() or c in " -_" else "_" for c in room_name).strip()
        png_bytes = self._last_image_bytes
        self._save_file_dialog(
            "Save Room Image", f"{safe_name}.png", "PNG images", "*.png",
            lambda path: self._save_png(png_bytes, path))

    def _save_room_as_mp4(self) -> None:
        """Save 3D room loop as MP4 (30fps, one full camera cycle)."""
        if not self._3d_renderer or not self._3d_renderer.available or not self._3d_params:
            self._status_label.set_text("3D renderer not available")
            return
        room_name = self._current_room_name or "room"
        safe_name = "".join(c if c.isalnum() or c in " -_" else "_" for c in room_name).strip()
        self._save_file_dialog(
            "Save 3D Room Loop", f"{safe_name}.mp4", "MP4 video", "*.mp4",
            lambda path: self._do_save_mp4(path))

    def _do_save_mp4(self, path: str) -> None:
        """Pause live 3D, render loop offline, encode to MP4, resume live 3D."""
        if not path.lower().endswith('.mp4'):
            path += '.mp4'
        renderer = self._3d_renderer
        params = self._3d_params
        w = renderer._width
        h = renderer._height

        # Calculate loop duration from camera speed
        speed = params.camera_speed if params.camera_speed > 0 else 0.5
        loop_duration = (2.0 * math.pi) / speed
        fps = 30
        total_frames = int(loop_duration * fps)
        if total_frames < 30:
            total_frames = 30
        if total_frames > 600:
            total_frames = 600

        # Pause the live 3D animation so we have exclusive renderer access
        self._stop_3d_animation(restore_static=False)
        self._status_label.set_text(f"Recording {total_frames} frames...")

        def _render_and_encode():
            try:
                proc = subprocess.Popen([
                    'ffmpeg', '-y',
                    '-f', 'rawvideo',
                    '-pix_fmt', 'rgba',
                    '-s', f'{w}x{h}',
                    '-r', str(fps),
                    '-i', 'pipe:0',
                    '-c:v', 'libx264',
                    '-pix_fmt', 'yuv420p',
                    '-preset', 'medium',
                    '-crf', '18',
                    '-movflags', '+faststart',
                    path,
                ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

                for i in range(total_frames):
                    t = (i / fps)
                    rgba = renderer.render_frame(t, params)
                    if rgba is None:
                        continue
                    proc.stdin.write(rgba)
                    if i % fps == 0:
                        GLib.idle_add(lambda i=i: self._status_label.set_text(
                            f"Recording frame {i}/{total_frames}...") or False)

                proc.stdin.close()
                proc.wait(timeout=30)

                name = Path(path).name
                GLib.idle_add(lambda: self._finish_mp4_export(name))
            except Exception as e:
                logger.error(f"MP4 export failed: {e}")
                GLib.idle_add(lambda: self._finish_mp4_export(None, str(e)))

        threading.Thread(target=_render_and_encode, daemon=True).start()

    def _finish_mp4_export(self, name: Optional[str], error: Optional[str] = None) -> None:
        """Resume live 3D after MP4 export completes."""
        if error:
            self._status_label.set_text(f"Export failed: {error}")
        else:
            self._status_label.set_text(f"Saved: {name}")
        # Resume live 3D animation
        if self._3d_enabled and self._3d_scene_ready:
            self._start_3d_animation()

    def _save_thumb_as_png(self, entity_name: str, thumb_key: str, base_name: str = "") -> None:
        """Save a thumbnail image as PNG."""
        png = None
        if self._get_thumb_fn:
            png = self._get_thumb_fn(thumb_key)
            if png is None and base_name:
                png = self._get_thumb_fn(base_name)
        if not png:
            self._status_label.set_text(f"No image for {entity_name}")
            return
        safe_name = "".join(c if c.isalnum() or c in " -_" else "_" for c in entity_name).strip()
        self._save_file_dialog(
            "Save Image", f"{safe_name}.png", "PNG images", "*.png",
            lambda path: self._save_png(png, path))

    # --- Context Menu Handlers ---

    def _on_room_image_right_drag(self, gesture, start_x, start_y):
        """Hold-right-click on room image."""
        items = []
        if self._3d_enabled and self._3d_scene_ready:
            items.append(("  Save as MP4 (3D loop)  ", self._save_room_as_mp4))
        items.append(("  Save as PNG  ", self._save_room_as_png))
        items.append(("  Regenerate Room Image  ", self._do_regenerate_room))
        self._hold_menu.show(items, gesture, start_x, start_y)

    def _do_regenerate_room(self):
        self._status_label.set_text("Regenerating room image...")
        if self._on_regenerate_room:
            self._on_regenerate_room()

    def _on_thumb_right_drag(self, gesture, start_x, start_y, entity_name,
                             thumb_key="", base_name=""):
        """Hold-right-click on NPC/monster thumbnail."""
        tk = thumb_key or entity_name
        bn = base_name
        self._hold_menu.show([
            (f"  Save as PNG  ",
             lambda: self._save_thumb_as_png(entity_name, tk, bn)),
            (f"  Look at {entity_name}  ",
             lambda: self._ctx_inject(f"l {entity_name}")),
            (f"  Attack {entity_name}  ",
             lambda: self._ctx_inject(f"a {entity_name}")),
            (f"  Regenerate thumbnail  ",
             lambda: self._do_regenerate_entity_by_name(entity_name)),
        ], gesture, start_x, start_y)

    def _do_regenerate_entity_by_name(self, name):
        if name and self._on_regenerate_entity:
            self._status_label.set_text(f"Regenerating {name}...")
            self._on_regenerate_entity(name)

    def _on_item_double_click(self, gesture, n_press, x, y, item_name):
        """Double-click on item thumbnail to pick it up."""
        if n_press == 2 and self._inject_fn:
            self._inject_fn(f"get {item_name}")
            GLib.timeout_add(500, lambda: self._inject_fn("i") or False)

    def _on_item_right_drag(self, gesture, start_x, start_y, item_name, thumb_key=""):
        """Hold-right-click on item thumbnail."""
        tk = thumb_key or item_name
        self._hold_menu.show([
            (f"  Save as PNG  ",
             lambda: self._save_thumb_as_png(item_name, tk)),
            (f"  Get {item_name}  ",
             lambda: self._ctx_inject_and_refresh(f"get {item_name}")),
            (f"  Look at {item_name}  ",
             lambda: self._ctx_inject(f"l {item_name}")),
            (f"  Regenerate Image  ",
             lambda: self._do_regenerate_entity_by_name(item_name)),
        ], gesture, start_x, start_y)

    # Map full direction names to MUD shorthand commands
    _DIR_ABBREV = {
        "north": "n", "south": "s", "east": "e", "west": "w",
        "northeast": "ne", "northwest": "nw",
        "southeast": "se", "southwest": "sw",
        "up": "u", "down": "d",
    }

    def _dir_cmd(self, direction: str) -> str:
        """Convert a direction name to its MUD command abbreviation."""
        return self._DIR_ABBREV.get(direction.lower(), direction)

    def _on_exit_left_click(self, gesture, n_press, x, y, direction):
        """Left-click on exit: go that direction."""
        if self._inject_fn:
            self._inject_fn(self._dir_cmd(direction))

    def _on_exit_right_drag(self, gesture, start_x, start_y, direction):
        """Hold-right-click on exit direction."""
        cmd = self._dir_cmd(direction)
        self._hold_menu.show([
            (f"  Go {direction}  ",
             lambda: self._ctx_inject(cmd)),
        ], gesture, start_x, start_y)

    def _ctx_inject(self, cmd: str) -> None:
        """Send a command via the inject callback."""
        if self._inject_fn:
            self._inject_fn(cmd)

    def _ctx_inject_and_refresh(self, cmd: str) -> None:
        """Send a command then refresh inventory."""
        if self._inject_fn:
            self._inject_fn(cmd)
            GLib.timeout_add(500, lambda: self._inject_fn("i") or False)

    # --- 3D Depth Parallax ---

    def set_3d_toggled_callback(self, cb: Callable[[bool], None]) -> None:
        self._on_3d_toggled_cb = cb

    def set_3d_renderer(self, renderer) -> None:
        """Set the VulkanDepthRenderer instance."""
        self._3d_renderer = renderer

    def set_3d_params(self, params) -> None:
        """Set DepthRenderParams for the parallax shader."""
        self._3d_params = params
        # Restart animation if it died (e.g. render error killed the tick)
        if self._3d_enabled and self._3d_scene_ready and self._3d_timer is None:
            self._start_3d_animation()

    def set_3d_enabled(self, enabled: bool) -> None:
        """Enable/disable 3D mode (called from main app). Avoids re-triggering callback."""
        act = self._view_actions.lookup_action("toggle-3d")
        if act and act.get_state().get_boolean() != enabled:
            act.change_state(GLib.Variant.new_boolean(enabled))

    def set_3d_scene_ready(self, ready: bool) -> None:
        """Mark whether the 3D depth scene is loaded for the current room."""
        self._3d_scene_ready = ready
        if ready and self._3d_enabled:
            self._has_first_image = True
            self._hide_placeholder()
            if self._3d_timer is None:
                GLib.idle_add(self._start_3d_animation)

    def invalidate_3d_scene(self) -> None:
        """Called only for brand new rooms with no cached image at all."""
        self._3d_scene_ready = False
        if self._3d_enabled:
            self._stop_3d_animation(restore_static=False)
            if not self._has_first_image:
                self._show_placeholder()

    # --- Menu action handlers ---

    def _on_action_3d(self, action, value) -> None:
        action.set_state(value)
        enabled = value.get_boolean()
        self._3d_enabled = enabled
        if self._on_3d_toggled_cb:
            self._on_3d_toggled_cb(enabled)
        if enabled:
            self._start_3d_animation()
        else:
            self._stop_3d_animation()

    def _on_action_scanlines(self, action, value) -> None:
        action.set_state(value)
        self._scanlines_enabled = value.get_boolean()
        self._save_view_config()
        if not self._3d_enabled and self._last_image_bytes:
            self.set_image(self._last_image_bytes)

    def _on_action_thickness(self, action, value) -> None:
        action.set_state(value)
        self._scanline_thickness = value.get_int32()
        self._save_view_config()
        if self._scanlines_enabled and not self._3d_enabled and self._last_image_bytes:
            self.set_image(self._last_image_bytes)

    def _on_action_warp(self, action, value) -> None:
        action.set_state(value)
        self._warp_enabled = value.get_boolean()
        self._save_view_config()
        if hasattr(self, '_char_window') and self._char_window:
            self._char_window.warp_enabled = self._warp_enabled

    def _on_action_console(self, action, value) -> None:
        action.set_state(value)
        self._show_console = value.get_boolean()
        self._console_scroll.set_visible(self._show_console)
        self._status_label.set_visible(self._show_console)
        self._save_view_config()

    def _on_action_monsters(self, action, value) -> None:
        action.set_state(value)
        self._show_monsters = value.get_boolean()
        self._save_view_config()
        # Only show if user enabled AND entities exist
        if not self._show_monsters:
            self._thumb_scroll.set_visible(False)
        elif self._thumb_widgets:
            self._thumb_scroll.set_visible(True)

    def _on_action_items(self, action, value) -> None:
        action.set_state(value)
        self._show_items = value.get_boolean()
        self._save_view_config()
        if not self._show_items:
            self._item_thumb_scroll.set_visible(False)
        elif self._item_thumb_widgets:
            self._item_thumb_scroll.set_visible(True)

    def _on_action_npc_location(self, action, value) -> None:
        action.set_state(value)
        self._npc_location = value.get_string()
        self._save_view_config()
        self._place_thumb_widgets()

    def _on_action_npc_lock(self, action, value) -> None:
        action.set_state(value)
        self._npc_locked = value.get_boolean()
        self._save_view_config()
        # Show/hide handle based on lock state and location
        if self._npc_location == "floating":
            self._npc_handle.set_visible(not self._npc_locked)
        else:
            self._npc_handle.set_visible(False)

    def _place_thumb_widgets(self) -> None:
        """(Re)position NPC and loot panels based on display location settings."""
        self._place_panel(self._npc_panel, self._npc_location,
                          self._npc_handle, self._npc_locked,
                          self._npc_float_x, self._npc_float_y)
        self._place_panel(self._loot_panel, self._loot_location,
                          self._loot_handle, self._loot_locked,
                          self._loot_float_x, self._loot_float_y)
        # Ensure combat effects overlay is always on top of everything
        self._combat_effects.raise_to_top()

    def _place_panel(self, panel, location, handle, locked, float_x, float_y):
        """Place a panel in vbox or as floating overlay."""
        parent = panel.get_parent()
        if parent == self._vbox:
            self._vbox.remove(panel)
        elif parent == self._root_overlay:
            self._root_overlay.remove_overlay(panel)

        panel.remove_css_class("npc-panel-floating")
        panel.set_halign(Gtk.Align.FILL)
        panel.set_valign(Gtk.Align.FILL)
        panel.set_margin_start(0)
        panel.set_margin_top(0)

        if location == "floating":
            panel.add_css_class("npc-panel-floating")
            panel.set_halign(Gtk.Align.START)
            panel.set_valign(Gtk.Align.START)
            panel.set_margin_start(int(float_x))
            panel.set_margin_top(int(float_y))
            handle.set_visible(not locked)
            self._root_overlay.add_overlay(panel)
        elif location == "below":
            handle.set_visible(False)
            self._vbox.insert_child_after(panel, self._image_overlay)
        else:
            # "above" (default)
            handle.set_visible(False)
            self._vbox.insert_child_after(panel, self._header_handle)

    def _on_action_loot_location(self, action, value) -> None:
        action.set_state(value)
        self._loot_location = value.get_string()
        self._save_view_config()
        self._place_thumb_widgets()

    def _on_action_loot_lock(self, action, value) -> None:
        action.set_state(value)
        self._loot_locked = value.get_boolean()
        self._save_view_config()
        if self._loot_location == "floating":
            self._loot_handle.set_visible(not self._loot_locked)
        else:
            self._loot_handle.set_visible(False)

    def _on_action_npc_scale(self, action, value) -> None:
        action.set_state(value)
        self._npc_thumb_scale = value.get_string()
        mult = self._SCALE_PCTS.get(self._npc_thumb_scale, 1.0)
        self._npc_thumb_size = max(16, int(self._NPC_BASE_SIZE * mult))
        self._save_view_config()
        if hasattr(self, '_last_npc_update_args') and self._last_npc_update_args:
            self.update_thumbnails(*self._last_npc_update_args)

    def _on_action_loot_scale(self, action, value) -> None:
        action.set_state(value)
        self._loot_thumb_scale = value.get_string()
        mult = self._SCALE_PCTS.get(self._loot_thumb_scale, 1.0)
        self._loot_thumb_size = max(16, int(self._LOOT_BASE_SIZE * mult))
        self._save_view_config()
        if hasattr(self, '_last_loot_update_args') and self._last_loot_update_args:
            self.update_item_thumbnails(*self._last_loot_update_args)

    def _on_action_dmg_scale(self, action, value) -> None:
        action.set_state(value)
        self._dmg_scale = value.get_string()
        self._dmg_scale_factor = self._SCALE_PCTS.get(self._dmg_scale, 1.0)
        self._combat_effects.set_font_scale(self._dmg_scale_factor)
        self._save_view_config()

    def _on_action_zdmg_scale(self, action, value) -> None:
        action.set_state(value)
        self._zdmg_scale = value.get_string()
        self._zdmg_scale_factor = self._SCALE_PCTS.get(self._zdmg_scale, 1.0)
        self._save_view_config()

    # --- Panel drag helpers ---

    def _panel_drag_begin(self, gesture, panel, location, locked, float_x, float_y):
        if location != "floating" or locked:
            gesture.reset()
            return False
        # Record pointer position at drag start (in panel-local coords)
        # and the panel's current position
        self._drag_panel = panel
        self._drag_start_panel_x = float_x
        self._drag_start_panel_y = float_y
        return True

    def _panel_drag_move(self, panel, start_x, start_y, offset_x, offset_y):
        """Move panel by offset from its start position.
        Defers the actual margin update to a frame tick to avoid multiple
        reflows per frame."""
        new_x = max(0, start_x + offset_x)
        new_y = max(0, start_y + offset_y)
        win_w = self.get_width()
        win_h = self.get_height()
        panel_w = panel.get_width()
        panel_h = panel.get_height()
        if win_w > 0 and panel_w > 0:
            new_x = min(new_x, win_w - min(panel_w, 80))
        if win_h > 0 and panel_h > 0:
            new_y = min(new_y, win_h - min(panel_h, 30))
        # Store pending position and schedule a tick callback if not already pending
        self._drag_pending_panel = panel
        self._drag_pending_x = int(new_x)
        self._drag_pending_y = int(new_y)
        if self._drag_tick_id is None:
            self._drag_tick_id = self.add_tick_callback(self._drag_tick)
        return new_x, new_y

    def _drag_tick(self, widget, frame_clock):
        """Apply pending drag position once per frame."""
        if self._drag_pending_panel:
            self._drag_pending_panel.set_margin_start(self._drag_pending_x)
            self._drag_pending_panel.set_margin_top(self._drag_pending_y)
        # Keep running while dragging
        if self._npc_dragging or self._loot_dragging:
            return True  # GLib.SOURCE_CONTINUE
        self._drag_tick_id = None
        return False  # GLib.SOURCE_REMOVE

    # --- NPC panel drag ---

    def _on_npc_drag_begin(self, gesture, start_x, start_y):
        if self._panel_drag_begin(gesture, self._npc_panel,
                                   self._npc_location, self._npc_locked,
                                   self._npc_float_x, self._npc_float_y):
            self._npc_dragging = True
            self._npc_drag_start_x = self._npc_float_x
            self._npc_drag_start_y = self._npc_float_y

    def _on_npc_drag_update(self, gesture, offset_x, offset_y):
        if not self._npc_dragging:
            return
        self._npc_float_x, self._npc_float_y = self._panel_drag_move(
            self._npc_panel, self._npc_drag_start_x, self._npc_drag_start_y,
            offset_x, offset_y)

    def _on_npc_drag_end(self, gesture, offset_x, offset_y):
        self._npc_dragging = False
        # Apply final position immediately
        if self._drag_pending_panel:
            self._drag_pending_panel.set_margin_start(self._drag_pending_x)
            self._drag_pending_panel.set_margin_top(self._drag_pending_y)
            self._drag_pending_panel = None
        self._save_view_config()

    # --- Loot panel drag ---

    def _on_loot_drag_begin(self, gesture, start_x, start_y):
        if self._panel_drag_begin(gesture, self._loot_panel,
                                   self._loot_location, self._loot_locked,
                                   self._loot_float_x, self._loot_float_y):
            self._loot_dragging = True
            self._loot_drag_start_x = self._loot_float_x
            self._loot_drag_start_y = self._loot_float_y

    def _on_loot_drag_update(self, gesture, offset_x, offset_y):
        if not self._loot_dragging:
            return
        self._loot_float_x, self._loot_float_y = self._panel_drag_move(
            self._loot_panel, self._loot_drag_start_x, self._loot_drag_start_y,
            offset_x, offset_y)

    def _on_loot_drag_end(self, gesture, offset_x, offset_y):
        self._loot_dragging = False
        if self._drag_pending_panel:
            self._drag_pending_panel.set_margin_start(self._drag_pending_x)
            self._drag_pending_panel.set_margin_top(self._drag_pending_y)
            self._drag_pending_panel = None
        self._save_view_config()

    def set_character_window(self, char_window) -> None:
        """Link the character window so settings can propagate."""
        self._char_window = char_window

    # --- Ctrl+hover description overlay ---

    def _on_key_pressed(self, ctrl, keyval, keycode, state):
        if keyval in (Gdk.KEY_Control_L, Gdk.KEY_Control_R):
            self._ctrl_held = True
            self._update_desc_overlay()

    def _on_key_released(self, ctrl, keyval, keycode, state):
        if keyval in (Gdk.KEY_Control_L, Gdk.KEY_Control_R):
            self._ctrl_held = False
            self._update_desc_overlay()

    def _on_image_hover_enter(self, ctrl, x, y):
        self._hovering_image = True
        self._update_desc_overlay()

    def _on_image_hover_leave(self, ctrl):
        self._hovering_image = False
        self._update_desc_overlay()

    def _update_desc_overlay(self):
        show = self._ctrl_held and self._hovering_image and bool(self._current_description)
        if show:
            self._desc_overlay_label.set_text(self._current_description)
        self._desc_overlay_box.set_visible(show)

    @staticmethod
    def _apply_scanlines(pixbuf, thickness: int = 2) -> GdkPixbuf.Pixbuf:
        """Apply CRT-style horizontal scanlines to a pixbuf."""
        import numpy as np
        w = pixbuf.get_width()
        h = pixbuf.get_height()
        n_channels = pixbuf.get_n_channels()
        rowstride = pixbuf.get_rowstride()
        pixels = pixbuf.get_pixels()

        # Convert to numpy array
        arr = np.frombuffer(pixels, dtype=np.uint8).copy()
        if rowstride == w * n_channels:
            arr = arr.reshape((h, w, n_channels))
        else:
            arr = np.array([
                arr[y * rowstride : y * rowstride + w * n_channels]
                for y in range(h)
            ], dtype=np.uint8).reshape((h, w, n_channels))

        # CRT scanline effect: black out every Nth row group
        # thickness controls how many px per dark line
        period = thickness * 2  # dark line + bright line
        for y in range(h):
            if (y % period) >= thickness:
                arr[y] = 0

        has_alpha = n_channels == 4
        return GdkPixbuf.Pixbuf.new_from_data(
            arr.tobytes(), GdkPixbuf.Colorspace.RGB,
            has_alpha, 8, w, h, w * n_channels,
        )

    def _start_3d_animation(self) -> None:
        """Start the 3D parallax render loop using frame clock for smooth animation."""
        if self._3d_timer is not None:
            return
        if not self._3d_renderer or not self._3d_renderer.available:
            self._status_label.set_text("3D mode unavailable (Vulkan renderer not loaded)")
            return
        if not self._3d_scene_ready:
            # Don't flash placeholder if we already have a scene displayed —
            # keep showing the old scene until the new one uploads
            if not self._has_first_image:
                self._show_placeholder()
            self._status_label.set_text("Loading 3D scene...")
            return
        self._hide_placeholder()
        self._3d_start_time = _time.monotonic()
        # Use widget tick callback — runs at monitor refresh rate even when unfocused
        self._3d_timer = self._image_old.add_tick_callback(self._3d_frame_clock_tick)
        self._status_label.set_text("3D Mode Active")

    def _stop_3d_animation(self, restore_static: bool = True) -> None:
        """Stop the 3D parallax render loop."""
        if self._3d_timer is not None:
            self._image_old.remove_tick_callback(self._3d_timer)
            self._3d_timer = None
        if restore_static:
            self._status_label.set_text("Ready")
            # Restore last static image
            if self._last_image_bytes:
                self.set_image(self._last_image_bytes)

    def _3d_frame_clock_tick(self, widget, frame_clock) -> bool:
        """Frame clock callback — runs at display refresh rate even when unfocused."""
        return self._3d_render_tick()

    def _3d_render_tick(self) -> bool:
        """Render one 3D parallax frame and update the display."""
        if not self._3d_enabled or not self._3d_renderer or not self._3d_params:
            logger.warning(f"3D tick stopping: enabled={self._3d_enabled} "
                          f"renderer={self._3d_renderer is not None} "
                          f"params={self._3d_params is not None}")
            self._3d_timer = None
            return False

        t = _time.monotonic() - self._3d_start_time
        try:
            rgba = self._3d_renderer.render_frame(t, self._3d_params)
            if rgba is None:
                return True  # keep trying

            w = self._3d_renderer._width
            h = self._3d_renderer._height
            # GdkPixbuf.new_from_bytes copies the data, avoiding GC issues
            gbytes = GLib.Bytes.new(rgba)
            pixbuf = GdkPixbuf.Pixbuf.new_from_bytes(
                gbytes, GdkPixbuf.Colorspace.RGB, True, 8,
                w, h, w * 4,
            )
            if self._scanlines_enabled:
                pixbuf = self._apply_scanlines(pixbuf, self._scanline_thickness)
            texture = Gdk.Texture.new_for_pixbuf(pixbuf)
            # Write to _image_old to avoid cross-fade conflicts
            self._image_old.set_paintable(texture)
            self._image_old.set_opacity(1.0)
            self._image_new.set_opacity(0.0)
        except Exception as e:
            logger.error(f"3D render tick error: {e}")

        return True  # continue

    def update_status(self, text: str) -> None:
        """Update the status bar text and log to console. Thread-safe."""
        def _do():
            self._status_label.set_text(text)
            self._console_log(text)
            return False
        GLib.idle_add(_do)

    def _console_log(self, text: str) -> None:
        """Append a line to the mini console log. Must be called from GTK thread."""
        end_iter = self._console_buffer.get_end_iter()
        if self._console_buffer.get_char_count() > 0:
            self._console_buffer.insert(end_iter, "\n")
            end_iter = self._console_buffer.get_end_iter()
        self._console_buffer.insert(end_iter, text)
        # Trim old lines
        line_count = self._console_buffer.get_line_count()
        if line_count > self._console_max_lines:
            start = self._console_buffer.get_start_iter()
            result = self._console_buffer.get_iter_at_line(line_count - self._console_max_lines)
            trim_end = result[1] if isinstance(result, tuple) else result
            self._console_buffer.delete(start, trim_end)
        # Auto-scroll to bottom
        self._console_scroll_to_end()

    def _console_scroll_to_end(self):
        end_mark = self._console_buffer.create_mark(None, self._console_buffer.get_end_iter(), False)
        self._console_text.scroll_to_mark(end_mark, 0, False, 0, 0)
        self._console_buffer.delete_mark(end_mark)

    def update_progress_bar(self, bar_id: str, label: str, fraction: float) -> None:
        """Update or create an animated progress bar line in the console. Thread-safe."""
        def _do():
            self._update_progress_bar_impl(bar_id, label, fraction)
            return False
        GLib.idle_add(_do)

    def _update_progress_bar_impl(self, bar_id: str, label: str, fraction: float):
        """Create or update a progress bar line in the console buffer."""
        bar_width = 30
        filled = int(fraction * bar_width)
        bar = "█" * filled + "░" * (bar_width - filled)
        pct = int(fraction * 100)
        line_text = f"{label} [{bar}] {pct}%"

        if bar_id in self._progress_bars:
            start_mark, end_mark = self._progress_bars[bar_id]
            # Check marks are still valid
            try:
                start_iter = self._console_buffer.get_iter_at_mark(start_mark)
                end_iter = self._console_buffer.get_iter_at_mark(end_mark)
                self._console_buffer.delete(start_iter, end_iter)
                start_iter = self._console_buffer.get_iter_at_mark(start_mark)
                self._console_buffer.insert(start_iter, line_text)
                # Move end mark to end of new text
                new_end = self._console_buffer.get_iter_at_mark(start_mark)
                new_end.forward_chars(len(line_text))
                self._console_buffer.move_mark(end_mark, new_end)
            except Exception:
                # Marks got invalidated, create new line
                del self._progress_bars[bar_id]
                self._create_progress_bar_line(bar_id, line_text)
        else:
            self._create_progress_bar_line(bar_id, line_text)

        self._console_scroll_to_end()

        # Clean up completed bars
        if fraction >= 1.0:
            if bar_id in self._progress_bars:
                start_mark, end_mark = self._progress_bars.pop(bar_id)
                self._console_buffer.delete_mark(start_mark)
                self._console_buffer.delete_mark(end_mark)

    def _create_progress_bar_line(self, bar_id: str, line_text: str):
        """Add a new progress bar line at the end of the console."""
        buf = self._console_buffer
        end_iter = buf.get_end_iter()
        if buf.get_char_count() > 0:
            buf.insert(end_iter, "\n")
            end_iter = buf.get_end_iter()
        # Create marks around the bar text
        start_mark = buf.create_mark(f"bar_{bar_id}_s", end_iter, True)
        buf.insert(end_iter, line_text)
        end_iter = buf.get_end_iter()
        end_mark = buf.create_mark(f"bar_{bar_id}_e", end_iter, False)
        self._progress_bars[bar_id] = (start_mark, end_mark)
