"""Offline asset baker — regenerates all entity thumbnails and depth maps at high quality.

Uses FLUX.2-klein-9B (full VRAM, no ollama needed) and optionally generates depth maps.
Outputs a .slop archive that the live app reads from before falling back to live generation.

Run as: ./python/bin/python3.12 -m mudproxy.bake
"""
import gc
import hashlib
import io
import json
import logging
import os
import struct
import sys
import time
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gtk, Gdk, GLib, Gio, Pango

logger = logging.getLogger(__name__)

CACHE_BASE = Path.home() / ".cache" / "mudproxy"
ENTITY_DIR = CACHE_BASE / "entities"
NPC_THUMB_DIR = CACHE_BASE / "npc"
ITEM_THUMB_DIR = CACHE_BASE / "item"
ROOM_CACHE_DIR = CACHE_BASE / "room"
DEPTH_CACHE_DIR = CACHE_BASE / "depth"
SLOP_DIR = Path.home() / ".local" / "share" / "mudproxy" / "slop"

OLLAMA_URL = "http://localhost:11434/api/generate"
OLLAMA_MODEL = "llama3.2:3b"

OLLAMA_MODELS = [
    "llama3.2:3b",
    "llama3.2:1b",
    "llama3.1:8b",
    "phi3:mini",
    "gemma2:2b",
    "gemma2:9b",
    "mistral:7b",
    "qwen2.5:3b",
    "qwen2.5:7b",
    "deepseek-r1:8b",
    "deepseek-v2:16b",
]

PORTRAIT_SYSTEM_PROMPT = (
    "You convert a text-game character/creature description into a concise portrait prompt "
    "for a fantasy art AI. Focus on: physical appearance, clothing, armor, weapons, "
    "facial features, body type, coloring. Output ONLY the portrait prompt as a single "
    "short paragraph. RESPOND IN ENGLISH ONLY. No thinking tags, no explanation, no Chinese, "
    "no other languages."
)

ITEM_SYSTEM_PROMPT = (
    "You convert a text-game item description into a concise image prompt "
    "for a fantasy art AI. ALWAYS start with the specific item type "
    "(e.g. 'A steel hatchet', 'A wooden longbow', 'A glass potion vial'). "
    "Focus on: the exact item type/weapon class, material, color, shape, "
    "magical effects, engravings, size. "
    "CRITICAL: Describe ONLY the object itself laid flat or floating. "
    "NEVER describe a person, figure, mannequin, or anyone wearing/holding it. "
    "For clothing/armor: describe the garment laid out flat as if on a table. "
    "Output ONLY the image prompt as a single short paragraph. "
    "RESPOND IN ENGLISH ONLY. No thinking tags, no explanation, no Chinese, no other languages."
)

# Available models for baking (high quality, uses full VRAM)
MODELS = {
    "flux-klein-4b": {
        "name": "FLUX.2 Klein 4B",
        "repo": "black-forest-labs/FLUX.2-klein-4B",
        "guidance": 0.0,
        "steps": 4,
        "pipeline": "flux",
    },
    "flux-klein-9b": {
        "name": "FLUX.2 Klein 9B",
        "repo": "black-forest-labs/FLUX.2-klein-9B",
        "guidance": 0.0,
        "steps": 4,
        "pipeline": "flux",
    },
    "flux-dev": {
        "name": "FLUX.2 Dev",
        "repo": "black-forest-labs/FLUX.2-dev",
        "guidance": 3.5,
        "steps": 20,
        "pipeline": "flux",
    },
    "flux-schnell-9b": {
        "name": "FLUX.1 Schnell",
        "repo": "black-forest-labs/FLUX.1-schnell",
        "guidance": 0.0,
        "steps": 4,
        "pipeline": "flux",
    },
    "sd15-rpg-v5": {
        "name": "SD 1.5 RPG v5",
        "repo": "Yntec/RPG",
        "lora": "latent-consistency/lcm-lora-sdv1-5",
        "guidance": 1.0,
        "steps": 4,
        "pipeline": "sd15-lcm",
        "width": 512,
        "height": 512,
        "negative": "blurry, low quality, watermark, text, modern, sci-fi, photograph",
    },
    "sd15-dreamshaper-v8": {
        "name": "SD 1.5 DreamShaper v8",
        "repo": "Lykon/dreamshaper-8",
        "lora": "latent-consistency/lcm-lora-sdv1-5",
        "guidance": 1.0,
        "steps": 4,
        "pipeline": "sd15-lcm",
        "width": 512,
        "height": 512,
        "negative": "blurry, low quality, watermark, text, modern, sci-fi, photograph",
    },
}


# ── SLOP file format ──────────────────────────────────────────────
# Header: b"SLOP" + u32 version + u32 entry_count
# Index:  per entry: u16 key_len + key_bytes + u8 asset_type + u64 offset + u32 size
# Data:   raw webp blobs
#
# asset_type: 0=npc_thumb, 1=item_thumb, 2=player_thumb, 3=room_image, 4=depth_map

SLOP_MAGIC = b"SLOP"
SLOP_VERSION = 1

ASSET_NPC_THUMB = 0
ASSET_ITEM_THUMB = 1
ASSET_PLAYER_THUMB = 2
ASSET_ROOM_IMAGE = 3
ASSET_DEPTH_MAP = 4
ASSET_PROMPT = 5
ASSET_METADATA = 6
ASSET_INPAINT = 7


def write_slop(path: Path, entries: dict[str, tuple[int, bytes]]) -> None:
    """Write a .slop archive.

    entries: {key: (asset_type, webp_bytes)}
    """
    with open(path, "wb") as f:
        # Header
        f.write(SLOP_MAGIC)
        f.write(struct.pack("<II", SLOP_VERSION, len(entries)))

        # Calculate index size first to get data offsets
        index_parts = []
        for key, (asset_type, _data) in entries.items():
            key_bytes = key.encode("utf-8")
            index_parts.append((key_bytes, asset_type))

        # Index size: per entry = 2 + key_len + 1 + 8 + 4 = 15 + key_len
        index_size = sum(2 + len(kb) + 1 + 8 + 4 for kb, _ in index_parts)
        header_size = 4 + 4 + 4  # magic + version + count
        data_start = header_size + index_size

        # Write index
        offset = data_start
        for (key, (asset_type, data)), (key_bytes, _) in zip(entries.items(), index_parts):
            f.write(struct.pack("<H", len(key_bytes)))
            f.write(key_bytes)
            f.write(struct.pack("<B", asset_type))
            f.write(struct.pack("<Q", offset))
            f.write(struct.pack("<I", len(data)))
            offset += len(data)

        # Write data
        for _key, (_asset_type, data) in entries.items():
            f.write(data)


def read_slop(path: Path) -> dict[str, tuple[int, bytes]]:
    """Read a .slop archive. Returns {key: (asset_type, webp_bytes)}."""
    entries = {}
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != SLOP_MAGIC:
            raise ValueError(f"Not a SLOP file: {path}")
        version, count = struct.unpack("<II", f.read(8))

        # Read index
        index = []
        for _ in range(count):
            key_len = struct.unpack("<H", f.read(2))[0]
            key = f.read(key_len).decode("utf-8")
            asset_type = struct.unpack("<B", f.read(1))[0]
            offset = struct.unpack("<Q", f.read(8))[0]
            size = struct.unpack("<I", f.read(4))[0]
            index.append((key, asset_type, offset, size))

        # Read data
        for key, asset_type, offset, size in index:
            f.seek(offset)
            data = f.read(size)
            entries[key] = (asset_type, data)

    return entries


@dataclass
class BakeJob:
    key: str  # entity name or room cache key
    name: str  # display name
    asset_type: int
    prompt: str  # image generation prompt
    width: int = 512
    height: int = 512
    done: bool = False
    error: str = ""
    image_bytes: Optional[bytes] = None


class BakeApp(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="com.mudproxy.bake")
        self._jobs: list[BakeJob] = []
        self._current_job_idx = 0
        self._running = False
        self._cancel = False
        self._pipe = None
        self._model_key = "flux-klein-9b"
        self._portrait_style = "fantasy art"
        self._gen_depth = False
        self._steps = 4
        self._thread: Optional[threading.Thread] = None

    def do_activate(self):
        win = Gtk.ApplicationWindow(application=self)
        win.set_title("MudProxy Asset Baker")
        win.set_default_size(700, 500)

        # CSS
        css = Gtk.CssProvider()
        css.load_from_string("""
            .bake-header { background: #1a1a2e; color: #e0e0e0; padding: 12px; }
            .bake-header label { color: #e0e0e0; }
            .bake-title { font-size: 18px; font-weight: bold; color: #7eb8da; }
            .bake-subtitle { font-size: 12px; color: #888; }
            .bake-stats { font-size: 13px; color: #aaa; padding: 8px 12px; }
            .bake-progress { min-height: 24px; }
            .bake-log { background: #0d0d1a; color: #c0c0c0; padding: 8px; font-family: monospace; font-size: 12px; }
            .bake-controls { padding: 8px 12px; }
            .bake-success { color: #4caf50; }
            .bake-error { color: #f44336; }
            .bake-active { color: #ffb74d; }
            window { background: #16162a; }
            .bake-combo { min-width: 150px; }
            .bake-combo-sm { min-width: 120px; }
        """)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        win.set_child(root)

        # ── Header ──
        header = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        header.add_css_class("bake-header")
        title = Gtk.Label(label="MudProxy Asset Baker")
        title.add_css_class("bake-title")
        title.set_halign(Gtk.Align.START)
        subtitle = Gtk.Label(label="Regenerate all entity thumbnails at maximum quality")
        subtitle.add_css_class("bake-subtitle")
        subtitle.set_halign(Gtk.Align.START)
        header.append(title)
        header.append(subtitle)
        root.append(header)

        # ── Controls row 1: Image model + steps + style ──
        controls1 = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        controls1.add_css_class("bake-controls")

        controls1.append(Gtk.Label(label="Image:"))
        self._model_combo = Gtk.DropDown.new_from_strings(
            [m["name"] for m in MODELS.values()]
        )
        self._model_combo.set_selected(0)
        self._model_combo.add_css_class("bake-combo")
        controls1.append(self._model_combo)

        controls1.append(Gtk.Label(label="Steps:"))
        self._steps_spin = Gtk.SpinButton.new_with_range(1, 50, 1)
        self._steps_spin.set_value(4)
        controls1.append(self._steps_spin)

        controls1.append(Gtk.Label(label="Style:"))
        self._style_entry = Gtk.Entry()
        self._style_entry.set_text("fantasy art")
        self._style_entry.set_hexpand(True)
        controls1.append(self._style_entry)

        root.append(controls1)

        # ── Controls row 2: Ollama prompt model ──
        controls2 = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        controls2.add_css_class("bake-controls")

        controls2.append(Gtk.Label(label="Prompt LLM:"))
        self._ollama_combo = Gtk.DropDown.new_from_strings(OLLAMA_MODELS)
        default_idx = OLLAMA_MODELS.index(OLLAMA_MODEL) if OLLAMA_MODEL in OLLAMA_MODELS else 0
        self._ollama_combo.set_selected(default_idx)
        self._ollama_combo.add_css_class("bake-combo-sm")
        controls2.append(self._ollama_combo)

        self._chk_regen_prompts = Gtk.CheckButton(label="Regenerate all prompts")
        self._chk_regen_prompts.set_active(False)
        controls2.append(self._chk_regen_prompts)

        root.append(controls2)

        # ── Filter checkboxes ──
        filter_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        filter_box.add_css_class("bake-controls")
        self._chk_npcs = Gtk.CheckButton(label="NPCs/Monsters")
        self._chk_npcs.set_active(True)
        self._chk_items = Gtk.CheckButton(label="Items")
        self._chk_items.set_active(True)
        self._chk_players = Gtk.CheckButton(label="Players")
        self._chk_players.set_active(True)
        self._chk_skip_existing = Gtk.CheckButton(label="Skip existing thumbnails")
        self._chk_skip_existing.set_active(False)
        filter_box.append(self._chk_npcs)
        filter_box.append(self._chk_items)
        filter_box.append(self._chk_players)
        filter_box.append(self._chk_skip_existing)
        root.append(filter_box)

        # ── Stats ──
        self._stats_label = Gtk.Label(label="Loading entities...")
        self._stats_label.add_css_class("bake-stats")
        self._stats_label.set_halign(Gtk.Align.START)
        root.append(self._stats_label)

        # ── Progress ──
        self._progress = Gtk.ProgressBar()
        self._progress.add_css_class("bake-progress")
        self._progress.set_show_text(True)
        self._progress.set_margin_start(12)
        self._progress.set_margin_end(12)
        root.append(self._progress)

        # ── Current item label ──
        self._current_label = Gtk.Label(label="")
        self._current_label.add_css_class("bake-active")
        self._current_label.set_halign(Gtk.Align.START)
        self._current_label.set_margin_start(12)
        root.append(self._current_label)

        # ── Log ──
        scroll = Gtk.ScrolledWindow()
        scroll.set_vexpand(True)
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        self._log_tv = Gtk.TextView()
        self._log_tv.set_editable(False)
        self._log_tv.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self._log_tv.add_css_class("bake-log")
        scroll.set_child(self._log_tv)
        root.append(scroll)

        # ── Buttons ──
        btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        btn_box.add_css_class("bake-controls")
        btn_box.set_halign(Gtk.Align.END)

        self._scan_btn = Gtk.Button(label="Scan Entities")
        self._scan_btn.connect("clicked", self._on_scan)
        btn_box.append(self._scan_btn)

        self._start_btn = Gtk.Button(label="Start Bake")
        self._start_btn.connect("clicked", self._on_start)
        self._start_btn.set_sensitive(False)
        btn_box.append(self._start_btn)

        self._cancel_btn = Gtk.Button(label="Cancel")
        self._cancel_btn.connect("clicked", self._on_cancel)
        self._cancel_btn.set_sensitive(False)
        btn_box.append(self._cancel_btn)

        self._save_btn = Gtk.Button(label="Save .slop")
        self._save_btn.connect("clicked", self._on_save_slop)
        self._save_btn.set_sensitive(False)
        btn_box.append(self._save_btn)

        self._merge_btn = Gtk.Button(label="Merge .slop Files")
        self._merge_btn.connect("clicked", self._on_merge_slop)
        btn_box.append(self._merge_btn)

        root.append(btn_box)

        # ── Slop file selector ──
        slop_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        slop_box.add_css_class("bake-controls")
        slop_box.append(Gtk.Label(label="Output .slop:"))
        self._slop_entry = Gtk.Entry()
        self._slop_entry.set_hexpand(True)
        self._slop_entry.set_placeholder_text("Auto-generated filename")
        slop_box.append(self._slop_entry)
        browse_btn = Gtk.Button(label="Browse...")
        browse_btn.connect("clicked", self._on_browse_slop)
        slop_box.append(browse_btn)
        root.append(slop_box)

        self._window = win
        win.present()

        # Auto-scan on launch
        GLib.idle_add(self._on_scan, None)

    def _log(self, msg: str, css_class: str = "") -> None:
        """Append a line to the log (thread-safe via GLib.idle_add)."""
        def _do():
            buf = self._log_tv.get_buffer()
            end = buf.get_end_iter()
            buf.insert(end, msg + "\n")
            # Auto-scroll
            mark = buf.create_mark(None, buf.get_end_iter(), False)
            self._log_tv.scroll_mark_onscreen(mark)
            buf.delete_mark(mark)
        GLib.idle_add(_do)

    def _set_progress(self, fraction: float, text: str = "") -> None:
        def _do():
            self._progress.set_fraction(fraction)
            if text:
                self._progress.set_text(text)
        GLib.idle_add(_do)

    def _set_current(self, text: str) -> None:
        GLib.idle_add(self._current_label.set_text, text)

    def _set_stats(self, text: str) -> None:
        GLib.idle_add(self._stats_label.set_text, text)

    def _on_scan(self, _btn) -> None:
        """Scan entity database and build job list."""
        self._jobs.clear()
        entities = {}

        # Load all entities from disk
        for f in ENTITY_DIR.glob("*.json"):
            try:
                data = json.loads(f.read_text())
                name = data.get("name", "")
                if name:
                    entities[name.lower()] = data
            except Exception:
                continue

        include_npcs = self._chk_npcs.get_active()
        include_items = self._chk_items.get_active()
        include_players = self._chk_players.get_active()
        skip_existing = self._chk_skip_existing.get_active()

        counts = {"npc": 0, "item": 0, "player": 0, "skipped": 0, "no_desc": 0}

        for name_lower, data in sorted(entities.items()):
            etype = data.get("entity_type", "npc")
            name = data.get("name", name_lower)
            desc = data.get("description", "")

            if not desc:
                counts["no_desc"] += 1
                continue

            # Filter by type
            if etype == "item" and not include_items:
                continue
            if etype == "player" and not include_players:
                continue
            if etype in ("npc", "creature") and not include_npcs:
                continue

            # Check existing thumbnail
            if skip_existing:
                safe = hashlib.sha256(name.lower().encode()).hexdigest()[:16]
                if etype == "item":
                    thumb_path = ITEM_THUMB_DIR / f"{safe}.webp"
                else:
                    thumb_path = NPC_THUMB_DIR / f"{safe}.webp"
                if thumb_path.exists():
                    counts["skipped"] += 1
                    continue

            # Determine asset type
            if etype == "item":
                asset_type = ASSET_ITEM_THUMB
                counts["item"] += 1
            elif etype == "player":
                asset_type = ASSET_PLAYER_THUMB
                counts["player"] += 1
            else:
                asset_type = ASSET_NPC_THUMB
                counts["npc"] += 1

            # Use existing base_prompt or flag for LLM generation
            prompt = data.get("base_prompt", "")

            self._jobs.append(BakeJob(
                key=name,
                name=name,
                asset_type=asset_type,
                prompt=prompt,  # empty = needs LLM prompt first
                width=512,
                height=512,
            ))

        total = counts["npc"] + counts["item"] + counts["player"]
        self._stats_label.set_text(
            f"Found {total} entities to bake: "
            f"{counts['npc']} NPCs, {counts['item']} items, {counts['player']} players "
            f"({counts['skipped']} skipped, {counts['no_desc']} without descriptions)"
        )
        self._log(f"Scanned {len(entities)} entities, {total} queued for baking")
        self._start_btn.set_sensitive(total > 0)
        self._current_job_idx = 0

    def _on_start(self, _btn) -> None:
        """Start the bake process."""
        if self._running:
            return
        self._running = True
        self._cancel = False
        self._current_job_idx = 0

        # Read settings
        model_idx = self._model_combo.get_selected()
        self._model_key = list(MODELS.keys())[model_idx]
        self._steps = int(self._steps_spin.get_value())
        self._portrait_style = self._style_entry.get_text().strip() or "fantasy art"

        # Ollama model selection
        ollama_idx = self._ollama_combo.get_selected()
        self._ollama_model = OLLAMA_MODELS[ollama_idx]

        # Regenerate all prompts checkbox — clear existing prompts to force regen
        if self._chk_regen_prompts.get_active():
            for job in self._jobs:
                job.prompt = ""

        self._start_btn.set_sensitive(False)
        self._scan_btn.set_sensitive(False)
        self._cancel_btn.set_sensitive(True)
        self._save_btn.set_sensitive(False)

        self._log(f"Starting bake with {MODELS[self._model_key]['name']}, "
                  f"{self._steps} steps, style='{self._portrait_style}', "
                  f"prompt LLM={self._ollama_model}")

        self._thread = threading.Thread(target=self._bake_worker, daemon=True)
        self._thread.start()

    def _on_cancel(self, _btn) -> None:
        self._cancel = True
        self._log("Cancelling...")
        self._cancel_btn.set_sensitive(False)

    def _on_save_slop(self, _btn) -> None:
        """Save completed jobs to a .slop file."""
        # Use user-selected path or auto-generate
        user_path = self._slop_entry.get_text().strip()
        if user_path:
            slop_path = Path(user_path)
            slop_path.parent.mkdir(parents=True, exist_ok=True)
        else:
            SLOP_DIR.mkdir(parents=True, exist_ok=True)
            timestamp = time.strftime("%Y%m%d_%H%M%S")
            slop_path = SLOP_DIR / f"bake_{timestamp}.slop"

        entries = {}

        # If appending to an existing file, load its entries first
        if slop_path.exists():
            try:
                existing = read_slop(slop_path)
                entries.update(existing)
                self._log(f"Appending to existing {slop_path.name} ({len(existing)} entries)")
            except Exception as e:
                self._log(f"Warning: could not read existing file: {e}")

        for job in self._jobs:
            if job.done and job.image_bytes:
                entries[job.key] = (job.asset_type, job.image_bytes)

        if not entries:
            self._log("No completed assets to save")
            return

        write_slop(slop_path, entries)
        self._log(f"Saved {len(entries)} assets to {slop_path}")
        self._log(f"File size: {slop_path.stat().st_size / 1024 / 1024:.1f} MB")

        # Update the entry field to show where we saved
        self._slop_entry.set_text(str(slop_path))

        # Also install thumbnails to the regular cache directories
        installed = 0
        for job in self._jobs:
            if not job.done or not job.image_bytes:
                continue
            safe = hashlib.sha256(job.key.lower().encode()).hexdigest()[:16]
            if job.asset_type == ASSET_ITEM_THUMB:
                dest = ITEM_THUMB_DIR / f"{safe}.webp"
            else:
                dest = NPC_THUMB_DIR / f"{safe}.webp"
            dest.write_bytes(job.image_bytes)
            installed += 1

        self._log(f"Installed {installed} thumbnails to cache directories")

    def _on_browse_slop(self, _btn) -> None:
        """Open file chooser for .slop output path."""
        dialog = Gtk.FileDialog()
        dialog.set_title("Choose .slop file")
        dialog.set_initial_folder(Gio.File.new_for_path(str(SLOP_DIR)))

        # Filter for .slop files
        slop_filter = Gtk.FileFilter()
        slop_filter.set_name("SLOP archives (*.slop)")
        slop_filter.add_pattern("*.slop")
        all_filter = Gtk.FileFilter()
        all_filter.set_name("All files")
        all_filter.add_pattern("*")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(slop_filter)
        filters.append(all_filter)
        dialog.set_filters(filters)

        dialog.save(self._window, None, self._on_browse_slop_done)

    def _on_browse_slop_done(self, dialog, result) -> None:
        try:
            gfile = dialog.save_finish(result)
            path = gfile.get_path()
            if path:
                if not path.endswith(".slop"):
                    path += ".slop"
                self._slop_entry.set_text(path)
                self._log(f"Output set to: {path}")
        except GLib.Error:
            pass  # user cancelled

    def _on_merge_slop(self, _btn) -> None:
        """Open file chooser to pick multiple .slop files to merge."""
        dialog = Gtk.FileDialog()
        dialog.set_title("Select .slop files to merge")
        dialog.set_initial_folder(Gio.File.new_for_path(str(SLOP_DIR)))

        slop_filter = Gtk.FileFilter()
        slop_filter.set_name("SLOP archives (*.slop)")
        slop_filter.add_pattern("*.slop")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(slop_filter)
        dialog.set_filters(filters)

        dialog.open_multiple(self._window, None, self._on_merge_select_done)

    def _on_merge_select_done(self, dialog, result) -> None:
        try:
            gfiles = dialog.open_multiple_finish(result)
        except GLib.Error:
            return  # user cancelled

        paths = []
        for i in range(gfiles.get_n_items()):
            gfile = gfiles.get_item(i)
            p = gfile.get_path()
            if p:
                paths.append(Path(p))

        if len(paths) < 2:
            self._log("Need at least 2 files to merge")
            return

        # Now pick the output destination
        out_dialog = Gtk.FileDialog()
        out_dialog.set_title("Save merged .slop as")
        out_dialog.set_initial_folder(Gio.File.new_for_path(str(SLOP_DIR)))
        out_dialog.set_initial_name("merged.slop")

        slop_filter = Gtk.FileFilter()
        slop_filter.set_name("SLOP archives (*.slop)")
        slop_filter.add_pattern("*.slop")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(slop_filter)
        out_dialog.set_filters(filters)

        # Stash paths for the callback
        self._merge_sources = paths
        out_dialog.save(self._window, None, self._on_merge_save_done)

    def _on_merge_save_done(self, dialog, result) -> None:
        try:
            gfile = dialog.save_finish(result)
            out_path = gfile.get_path()
        except GLib.Error:
            return  # user cancelled

        if not out_path:
            return
        if not out_path.endswith(".slop"):
            out_path += ".slop"
        out_path = Path(out_path)

        merged = {}
        total_files = 0
        for src in self._merge_sources:
            try:
                entries = read_slop(src)
                before = len(merged)
                merged.update(entries)
                added = len(merged) - before
                self._log(f"  {src.name}: {len(entries)} entries ({added} new)")
                total_files += 1
            except Exception as e:
                self._log(f"  Error reading {src.name}: {e}")

        if not merged:
            self._log("No entries to merge")
            return

        write_slop(out_path, merged)
        self._log(f"Merged {total_files} files → {out_path.name}: "
                   f"{len(merged)} entries, {out_path.stat().st_size / 1024 / 1024:.1f} MB")
        self._slop_entry.set_text(str(out_path))
        self._merge_sources = []

    def _generate_prompt_ollama(self, entity_data: dict) -> str:
        """Call ollama to generate an image prompt from entity description."""
        import urllib.request

        etype = entity_data.get("entity_type", "npc")
        system = ITEM_SYSTEM_PROMPT if etype == "item" else PORTRAIT_SYSTEM_PROMPT

        prompt_text = f"Name: {entity_data['name']}\nDescription: {entity_data['description']}"
        if etype == "player" and entity_data.get("equipment"):
            equip_str = ", ".join(entity_data["equipment"])
            prompt_text += f"\nEquipped items: {equip_str}"

        model = getattr(self, '_ollama_model', OLLAMA_MODEL)
        payload = json.dumps({
            "model": model,
            "system": system,
            "prompt": prompt_text,
            "stream": False,
        }).encode()

        req = urllib.request.Request(
            OLLAMA_URL,
            data=payload,
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.loads(resp.read().decode())

        response = data.get("response", "").strip()
        if "<think>" in response:
            parts = response.split("</think>")
            response = parts[-1].strip()
        response = response.strip("\"'")
        return response

    @staticmethod
    def _validate_prompt(prompt: str) -> tuple[bool, str]:
        """Check if a prompt is valid English text suitable for image generation.
        Returns (ok, reason)."""
        if not prompt or len(prompt) < 20:
            return False, "too short"

        # Count non-ASCII characters (CJK, Cyrillic, Arabic, etc.)
        non_ascii = sum(1 for c in prompt if ord(c) > 127)
        ratio = non_ascii / len(prompt)
        if ratio > 0.15:
            return False, f"non-English ({non_ascii}/{len(prompt)} non-ASCII chars, {ratio:.0%})"

        # Check for CJK character ranges specifically
        cjk_count = sum(1 for c in prompt if '\u4e00' <= c <= '\u9fff'  # CJK Unified
                        or '\u3000' <= c <= '\u303f'  # CJK Punctuation
                        or '\u3040' <= c <= '\u309f'  # Hiragana
                        or '\u30a0' <= c <= '\u30ff'  # Katakana
                        or '\uff00' <= c <= '\uffef')  # Fullwidth
        if cjk_count > 3:
            return False, f"contains CJK characters ({cjk_count} found)"

        # Check for excessive repetition (model looping)
        words = prompt.lower().split()
        if len(words) > 10:
            from collections import Counter
            counts = Counter(words)
            most_common_word, most_common_count = counts.most_common(1)[0]
            if most_common_count > len(words) * 0.3 and most_common_count > 5:
                return False, f"repetitive ('{most_common_word}' appears {most_common_count}x)"

        # Check for thinking tags that weren't stripped
        if "<think>" in prompt.lower() or "</think>" in prompt.lower():
            return False, "contains thinking tags"

        return True, "ok"

    def _build_thumb_prompt(self, base_prompt: str, entity_data: dict) -> str:
        """Build the full thumbnail prompt with suffix."""
        etype = entity_data.get("entity_type", "npc")
        if etype == "item":
            return (f"{base_prompt}, single object laid flat on dark background, "
                    f"no person, no mannequin, no figure, item icon, fantasy art, detailed")
        else:
            style = self._portrait_style
            return f"{base_prompt}, portrait, square frame, {style}, detailed"

    def _bake_worker(self) -> None:
        """Background thread: generate all prompts via ollama, then generate all images."""
        import torch

        total = len(self._jobs)

        # ── Phase 1: Generate prompts via ollama for entities that need them ──
        self._log("── Phase 1: Generating image prompts via Ollama ──")
        need_prompts = [(i, j) for i, j in enumerate(self._jobs) if not j.prompt]

        if need_prompts:
            self._set_progress(0, f"Generating prompts: 0/{len(need_prompts)}")
            for pi, (ji, job) in enumerate(need_prompts):
                if self._cancel:
                    break

                self._set_current(f"LLM prompt: {job.name}")

                # Load entity data for prompt generation
                safe = hashlib.sha256(job.key.lower().encode()).hexdigest()[:16]
                entity_file = ENTITY_DIR / f"{safe}.json"
                try:
                    entity_data = json.loads(entity_file.read_text())
                except Exception:
                    self._log(f"  [!] Cannot read entity: {job.name}")
                    job.error = "missing entity file"
                    continue

                try:
                    prompt = self._generate_prompt_ollama(entity_data)
                    ok, reason = self._validate_prompt(prompt)
                    if not ok:
                        # Retry once
                        self._log(f"  [retry] {job.name}: {reason}, retrying...")
                        prompt = self._generate_prompt_ollama(entity_data)
                        ok, reason = self._validate_prompt(prompt)

                    if ok:
                        job.prompt = prompt
                        entity_data["base_prompt"] = prompt
                        entity_file.write_text(json.dumps(entity_data))
                        self._log(f"  [ok] {job.name}: {prompt[:60]}...")
                    else:
                        self._log(f"  [!] {job.name}: {reason} — \"{prompt[:60]}\"")
                        job.error = f"bad prompt: {reason}"
                except Exception as e:
                    self._log(f"  [!] LLM failed for {job.name}: {e}")
                    job.error = str(e)

                frac = (pi + 1) / len(need_prompts)
                self._set_progress(frac, f"Generating prompts: {pi + 1}/{len(need_prompts)}")
        else:
            self._log("  All entities already have prompts")

        if self._cancel:
            self._finish_bake("Cancelled during prompt generation")
            return

        # ── Phase 1.5: Validate all prompts ──
        self._log("── Validating prompts ──")
        bad_prompts = []
        for job in self._jobs:
            if not job.prompt or job.error:
                continue
            ok, reason = self._validate_prompt(job.prompt)
            if not ok:
                bad_prompts.append((job, reason))
                job.error = f"bad prompt: {reason}"
                self._log(f"  [BAD] {job.name}: {reason} — \"{job.prompt[:80]}\"")

                # Clear the bad prompt from the entity json so it regenerates next time
                safe = hashlib.sha256(job.key.lower().encode()).hexdigest()[:16]
                entity_file = ENTITY_DIR / f"{safe}.json"
                try:
                    entity_data = json.loads(entity_file.read_text())
                    if "base_prompt" in entity_data:
                        del entity_data["base_prompt"]
                        entity_file.write_text(json.dumps(entity_data))
                except Exception:
                    pass

        if bad_prompts:
            self._log(f"  Rejected {len(bad_prompts)} bad prompts (will retry next bake)")
        else:
            self._log("  All prompts OK")

        # ── Phase 2: Unload ollama, load FLUX ──
        self._log("── Phase 2: Loading image model ──")
        self._set_current("Unloading Ollama...")
        self._set_progress(0, "Loading model...")

        # Unload ollama
        try:
            import requests
            ps = requests.get("http://localhost:11434/api/ps", timeout=5).json()
            for m in ps.get("models", []):
                model_name = m.get("name", "")
                requests.post(
                    "http://localhost:11434/api/generate",
                    json={"model": model_name, "keep_alive": 0},
                    timeout=10,
                )
                self._log(f"  Unloaded {model_name} from Ollama")
            if ps.get("models"):
                time.sleep(2)
        except Exception as e:
            self._log(f"  Warning: Could not unload Ollama: {e}")

        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()

        # Load FLUX
        model_cfg = MODELS[self._model_key]
        self._set_current(f"Loading {model_cfg['name']}...")
        self._log(f"  Loading {model_cfg['name']} from {model_cfg['repo']}...")

        device = "cuda" if torch.cuda.is_available() else "cpu"
        dtype = torch.bfloat16 if device == "cuda" else torch.float32

        has_flash = False
        try:
            import flash_attn  # noqa: F401
            has_flash = True
        except ImportError:
            pass

        load_kwargs = {"torch_dtype": dtype}
        if has_flash:
            load_kwargs["attn_implementation"] = "flash_attention_2"

        try:
            from diffusers import FluxPipeline
            self._pipe = FluxPipeline.from_pretrained(
                model_cfg["repo"], **load_kwargs
            )
        except Exception:
            from diffusers import DiffusionPipeline
            self._pipe = DiffusionPipeline.from_pretrained(
                model_cfg["repo"], **load_kwargs
            )
        self._pipe = self._pipe.to(device)
        attn_str = " + Flash Attention 2" if has_flash else ""
        self._log(f"  {model_cfg['name']} loaded on {device}{attn_str}")

        if self._cancel:
            self._unload_pipe()
            self._finish_bake("Cancelled during model load")
            return

        # ── Phase 3: Generate images ──
        self._log("── Phase 3: Generating images ──")
        generated = 0
        errors = 0

        ready_jobs = [(i, j) for i, j in enumerate(self._jobs)
                      if j.prompt and not j.error and not j.done]

        for pi, (ji, job) in enumerate(ready_jobs):
            if self._cancel:
                break

            self._set_current(f"Generating: {job.name} ({pi + 1}/{len(ready_jobs)})")

            # Load entity data for prompt suffix
            safe = hashlib.sha256(job.key.lower().encode()).hexdigest()[:16]
            entity_file = ENTITY_DIR / f"{safe}.json"
            try:
                entity_data = json.loads(entity_file.read_text())
            except Exception:
                entity_data = {"entity_type": "npc"}

            full_prompt = self._build_thumb_prompt(job.prompt, entity_data)

            try:
                import random
                seed = random.randint(0, 2**32 - 1)
                generator = torch.Generator(device=device).manual_seed(seed)

                steps = self._steps

                def _step_cb(pipe, step, timestep, kwargs):
                    step_frac = (step + 1) / steps
                    overall = (pi + step_frac) / len(ready_jobs)
                    self._set_progress(
                        overall,
                        f"Image {pi + 1}/{len(ready_jobs)}: {job.name} "
                        f"(step {step + 1}/{steps})"
                    )
                    return kwargs

                with torch.no_grad():
                    image = self._pipe(
                        prompt=full_prompt,
                        width=job.width,
                        height=job.height,
                        num_inference_steps=steps,
                        guidance_scale=model_cfg["guidance"],
                        generator=generator,
                        callback_on_step_end=_step_cb,
                    ).images[0]

                buf = io.BytesIO()
                image.save(buf, format="WebP", quality=90)
                job.image_bytes = buf.getvalue()
                job.done = True
                generated += 1

                self._log(f"  [ok] {job.name} ({len(job.image_bytes) / 1024:.0f} KB)")

            except Exception as e:
                job.error = str(e)
                errors += 1
                self._log(f"  [!] {job.name}: {e}")

            frac = (pi + 1) / len(ready_jobs)
            self._set_progress(frac, f"Generated {pi + 1}/{len(ready_jobs)}")

        # ── Cleanup ──
        self._unload_pipe()

        status = "Cancelled" if self._cancel else "Complete"
        self._finish_bake(
            f"{status}: {generated} generated, {errors} errors, "
            f"{len(self._jobs) - generated - errors} skipped"
        )

    def _unload_pipe(self) -> None:
        if self._pipe is not None:
            import torch
            self._pipe.to("cpu")
            del self._pipe
            self._pipe = None
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()

    def _finish_bake(self, msg: str) -> None:
        self._log(f"── {msg} ──")
        self._set_current(msg)
        self._running = False

        def _ui():
            self._start_btn.set_sensitive(True)
            self._scan_btn.set_sensitive(True)
            self._cancel_btn.set_sensitive(False)
            has_results = any(j.done and j.image_bytes for j in self._jobs)
            self._save_btn.set_sensitive(has_results)
        GLib.idle_add(_ui)


def main():
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    app = BakeApp()
    app.run(sys.argv)


if __name__ == "__main__":
    main()
