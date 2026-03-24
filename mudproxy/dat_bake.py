"""Batch bake MajorMUD 1.11p .dat files into a .slop archive.

Parses Btrieve .dat files (monsters, items, rooms) using record structures from
Nightmare Redux, generates image prompts via Ollama, renders images via FLUX,
processes depth maps, and packages everything into a .slop file.

Run as: ./python/bin/python3.12 -m mudproxy.dat_bake /path/to/dat/directory
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
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import Gtk, Gdk, GdkPixbuf, GLib, Gio, Pango

from .bake import (
    write_slop, read_slop, SLOP_DIR, MODELS, OLLAMA_URL, OLLAMA_MODEL, OLLAMA_MODELS,
    PORTRAIT_SYSTEM_PROMPT, ITEM_SYSTEM_PROMPT, BakeApp,
    ASSET_NPC_THUMB, ASSET_ITEM_THUMB, ASSET_ROOM_IMAGE, ASSET_DEPTH_MAP,
    ASSET_PROMPT, ASSET_METADATA, ASSET_INPAINT,
)

logger = logging.getLogger(__name__)

CACHE_BASE = Path.home() / ".cache" / "mudproxy"
PROMPT_CACHE_DIR = CACHE_BASE / "bake_prompts"

# Room image prompt system prompt (same as room_renderer)
ROOM_SYSTEM_PROMPT = (
    "You convert MUD/text-game room descriptions into concise image generation prompts "
    "for a fantasy art AI. The game is set in a MEDIEVAL FANTASY world — all streets are "
    "cobblestone paths, all buildings are stone/timber/thatch, all lighting is torches/lanterns/"
    "candles. There are NO modern elements whatsoever. "
    "Extract ONLY visual elements: setting, lighting, objects, atmosphere, architecture. "
    "NEVER include people, figures, characters, NPCs, or any living beings in the prompt. "
    "The scene should be EMPTY of people — show only the environment/room itself. "
    "Output ONLY the image prompt as a single short paragraph. No thinking tags, no explanation."
)

ROOM_STYLE_SUFFIX = (
    "fantasy art, detailed environment painting, atmospheric lighting, "
    "medieval RPG, muted earthy tones, painterly style"
)

ROOM_STYLE_SUFFIX_3D = (
    "photorealistic fantasy scene, cinematic depth of field, volumetric lighting, "
    "strong foreground midground background separation, atmospheric perspective, "
    "dramatic parallax depth layers, high detail textures, ray traced lighting, "
    "medieval dark fantasy, immersive 3D environment, wide angle perspective"
)


# ── Btrieve .dat parser ─────────────────────────────────────────────

def _iter_btrieve_records(path: Path, page_size: int, rec_len: int,
                          phys_len: int, hdr_size: int = 8):
    """Iterate over all records in a Btrieve .dat file.

    Yields raw bytes of each record (rec_len bytes).
    Skips FCR, ACS, and key/index pages — only reads data pages
    (identified by bit 7 set in byte 5 of the page header).
    """
    data = path.read_bytes()
    num_pages = len(data) // page_size

    for pg in range(1, num_pages):
        offset = pg * page_size
        if offset + page_size > len(data):
            break
        page = data[offset:offset + page_size]
        # Data pages have bit 7 set at byte 5
        if not (page[5] & 0x80):
            continue
        for rec_off in range(hdr_size, page_size - rec_len, phys_len):
            rec = page[rec_off:rec_off + rec_len]
            if len(rec) < rec_len:
                break
            yield rec


def _decode_str(raw: bytes) -> str:
    """Decode a null-terminated CP437 string."""
    return raw.split(b'\x00')[0].decode('cp437', errors='replace').strip()


# ── Record parsers ───────────────────────────────────────────────────

def parse_monsters(dat_path: Path) -> list[dict]:
    """Parse wccknms2.dat → list of monster dicts."""
    monsters = []
    seen = set()
    for rec in _iter_btrieve_records(dat_path, page_size=1536, rec_len=756,
                                     phys_len=758, hdr_size=8):
        num = struct.unpack_from('<i', rec, 0)[0]
        if num <= 0 or num in seen:
            continue
        name = _decode_str(rec[54:83])
        if not name or not name.isprintable():
            continue
        seen.add(num)

        # Description lines (4 lines × 70 chars at offsets 468, 539, 610, 681)
        desc_parts = []
        for off in (468, 539, 610, 681):
            line = _decode_str(rec[off:off + 70])
            if line:
                desc_parts.append(line)
        desc = ' '.join(desc_parts)

        hp = struct.unpack_from('<h', rec, 120)[0]
        exp = struct.unpack_from('<i', rec, 116)[0]
        ac = struct.unpack_from('<h', rec, 106)[0]
        dr = struct.unpack_from('<h', rec, 104)[0]
        mr = struct.unpack_from('<h', rec, 112)[0]

        monsters.append({
            "num": num, "name": name, "desc": desc,
            "hp": hp, "exp": exp, "ac": ac, "dr": dr, "mr": mr,
        })

    monsters.sort(key=lambda m: m["num"])
    return monsters


def parse_items(dat_path: Path) -> list[dict]:
    """Parse wccitem2.dat → list of item dicts."""
    items = []
    seen = set()
    for rec in _iter_btrieve_records(dat_path, page_size=4096, rec_len=1072,
                                     phys_len=1072, hdr_size=6):
        num = struct.unpack_from('<i', rec, 0)[0]
        if num <= 0 or num in seen:
            continue
        name = _decode_str(rec[173:202])
        if not name or not name.isprintable():
            continue
        seen.add(num)

        # Description lines (6 lines × 60 chars at offsets 203, 264, 325, 386, 447, 508)
        desc_parts = []
        for off in (203, 264, 325, 386, 447, 508):
            line = _decode_str(rec[off:off + 60])
            if line:
                desc_parts.append(line)
        desc = ' '.join(desc_parts)

        item_type = struct.unpack_from('<h', rec, 756)[0]
        weight = struct.unpack_from('<h', rec, 754)[0]

        items.append({
            "num": num, "name": name, "desc": desc,
            "type": item_type, "weight": weight,
        })

    items.sort(key=lambda i: i["num"])
    return items


_EXIT_DIRS = ["north", "south", "east", "west", "northeast", "northwest",
              "southeast", "southwest", "up", "down"]


def parse_rooms(dat_path: Path) -> list[dict]:
    """Parse wccmp002.dat → list of room dicts with exits."""
    rooms = []
    seen = set()
    for rec in _iter_btrieve_records(dat_path, page_size=4096, rec_len=1544,
                                     phys_len=1546, hdr_size=8):
        map_num = struct.unpack_from('<i', rec, 0)[0]
        room_num = struct.unpack_from('<i', rec, 4)[0]
        if map_num <= 0:
            continue
        key = f"{map_num}-{room_num}"
        if key in seen:
            continue
        seen.add(key)

        name = _decode_str(rec[261:314])
        if not name or not name.isprintable():
            continue

        # 7 description lines × 71 chars at offset 314
        desc_parts = []
        for i in range(7):
            line = _decode_str(rec[314 + i * 71:314 + (i + 1) * 71])
            if line:
                desc_parts.append(line)
        desc = ' '.join(desc_parts)

        # RoomExit(9) — 10 Longs at offset 824
        exits = []
        for i in range(10):
            exit_room = struct.unpack_from('<i', rec, 824 + i * 4)[0]
            if exit_room > 0:
                exits.append(_EXIT_DIRS[i])

        rooms.append({
            "map": map_num, "room": room_num, "key": key,
            "name": name, "desc": desc, "exits": exits,
        })

    rooms.sort(key=lambda r: (r["map"], r["room"]))
    return rooms


# ── Bake job ─────────────────────────────────────────────────────────

@dataclass
class DatBakeJob:
    key: str
    name: str
    asset_type: int
    description: str
    prompt: str = ""
    model_key: str = "flux-klein-9b"
    width: int = 512
    height: int = 512
    done: bool = False
    error: str = ""
    image_bytes: Optional[bytes] = None
    depth_bytes: Optional[bytes] = None
    inpaint_bytes: Optional[bytes] = None
    room_meta: Optional[dict] = None  # {map, room, name, exits} for rooms


ASSET_TYPE_NAMES = {
    ASSET_NPC_THUMB: "NPC", ASSET_ITEM_THUMB: "Item", ASSET_ROOM_IMAGE: "Room",
    ASSET_DEPTH_MAP: "Depth", ASSET_INPAINT: "Inpaint", ASSET_PROMPT: "Prompt",
    ASSET_METADATA: "Meta",
}


# ── Merge Dialog ───────────────────────────────────────────────────

class MergeDialog(Gtk.Window):
    """Preview merge contents, resolve conflicts, pick output path, merge."""

    def __init__(self, parent, file_entries: dict[Path, dict[str, tuple[int, bytes]]], app=None):
        super().__init__(title="Merge .slop Files", transient_for=parent, modal=True)
        self.set_default_size(750, 550)
        self._file_entries = file_entries  # {path: {key: (type, data)}}
        self._conflicts = {}   # {key: [(path, type, data), ...]}
        self._resolved = {}    # {key: (type, data)} — user's picks for conflicts
        self._app = app  # DatBakeApp instance for logging

        # Detect conflicts: same key, different data
        all_keys = {}  # key -> [(path, type, data)]
        for path, entries in file_entries.items():
            for key, (atype, data) in entries.items():
                all_keys.setdefault(key, []).append((path, atype, data))

        for key, sources in all_keys.items():
            if len(sources) > 1:
                # Check if data actually differs
                first_data = sources[0][2]
                if any(s[2] != first_data for s in sources[1:]):
                    self._conflicts[key] = sources

        self._build_ui()

    def _count_types(self, entries):
        from collections import Counter
        c = Counter(atype for atype, _ in entries.values())
        parts = []
        for t in (ASSET_NPC_THUMB, ASSET_ITEM_THUMB, ASSET_ROOM_IMAGE,
                  ASSET_DEPTH_MAP, ASSET_PROMPT):
            if c[t]:
                parts.append(f"{c[t]} {ASSET_TYPE_NAMES.get(t, '?')}")
        return ", ".join(parts) or "empty"

    def _build_ui(self):
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        vbox.set_margin_start(12)
        vbox.set_margin_end(12)
        vbox.set_margin_top(12)
        vbox.set_margin_bottom(12)
        self.set_child(vbox)

        # ── File summaries ──
        lbl = Gtk.Label(label="<b>Files to merge:</b>", use_markup=True)
        lbl.set_halign(Gtk.Align.START)
        vbox.append(lbl)

        for path, entries in self._file_entries.items():
            size_mb = path.stat().st_size / 1024 / 1024
            summary = self._count_types(entries)
            row = Gtk.Label(
                label=f"  {path.name}  ({size_mb:.1f} MB) — {len(entries)} entries: {summary}",
            )
            row.set_halign(Gtk.Align.START)
            row.set_selectable(True)
            vbox.append(row)

        # ── Totals ──
        all_keys = set()
        for entries in self._file_entries.values():
            all_keys.update(entries.keys())

        total_lbl = Gtk.Label(
            label=f"\n<b>Result:</b> {len(all_keys)} unique entries, "
                  f"<b>{len(self._conflicts)} conflicts</b>",
            use_markup=True,
        )
        total_lbl.set_halign(Gtk.Align.START)
        vbox.append(total_lbl)

        # ── Conflicts ──
        if self._conflicts:
            sep = Gtk.Separator()
            vbox.append(sep)

            clbl = Gtk.Label(
                label="<b>Conflicts</b> — same key, different data. Pick which to keep:",
                use_markup=True,
            )
            clbl.set_halign(Gtk.Align.START)
            vbox.append(clbl)

            scroll = Gtk.ScrolledWindow()
            scroll.set_vexpand(True)
            scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
            conflict_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
            scroll.set_child(conflict_box)
            vbox.append(scroll)

            self._conflict_combos = {}
            for key, sources in sorted(self._conflicts.items()):
                row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)

                # Key name + type
                atype = sources[0][1]
                type_name = ASSET_TYPE_NAMES.get(atype, "?")
                display_key = key[:40] + "..." if len(key) > 40 else key
                key_lbl = Gtk.Label(label=f"[{type_name}] {display_key}")
                key_lbl.set_halign(Gtk.Align.START)
                key_lbl.set_hexpand(True)
                key_lbl.set_ellipsize(Pango.EllipsizeMode.END)
                row.append(key_lbl)

                # Size info for each source
                combo = Gtk.ComboBoxText()
                for i, (src_path, src_type, src_data) in enumerate(sources):
                    size_str = f"{len(src_data)}" if len(src_data) < 1024 else f"{len(src_data)/1024:.0f}K"
                    label = f"{src_path.name} ({size_str})"
                    # For prompts, show a preview
                    if src_type == ASSET_PROMPT:
                        preview = src_data.decode("utf-8", errors="replace")[:50]
                        label = f"{src_path.name}: \"{preview}...\""
                    combo.append(str(i), label)
                combo.set_active(len(sources) - 1)  # default: newest (last file)
                self._conflict_combos[key] = (combo, sources)
                row.append(combo)

                # Preview button for images
                if atype in (ASSET_NPC_THUMB, ASSET_ITEM_THUMB, ASSET_ROOM_IMAGE):
                    preview_btn = Gtk.Button(label="Compare")
                    preview_btn.connect("clicked", self._on_compare, key, sources)
                    row.append(preview_btn)

                conflict_box.append(row)
        else:
            no_conflict = Gtk.Label(label="\nNo conflicts — all entries are unique or identical.")
            no_conflict.set_halign(Gtk.Align.START)
            vbox.append(no_conflict)
            # Add spacer
            spacer = Gtk.Box()
            spacer.set_vexpand(True)
            vbox.append(spacer)

        # ── Buttons ──
        btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        btn_box.set_halign(Gtk.Align.END)
        btn_box.set_margin_top(8)

        cancel_btn = Gtk.Button(label="Cancel")
        cancel_btn.connect("clicked", lambda _: self.close())
        btn_box.append(cancel_btn)

        merge_btn = Gtk.Button(label="Merge && Save")
        merge_btn.connect("clicked", self._on_do_merge)
        merge_btn.add_css_class("suggested-action")
        btn_box.append(merge_btn)

        vbox.append(btn_box)

    def _on_compare(self, _btn, key, sources):
        """Show a popup comparing images side-by-side."""
        win = Gtk.Window(title=f"Compare: {key}", transient_for=self, modal=True)
        win.set_default_size(500, 300)

        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        box.set_margin_start(8)
        box.set_margin_end(8)
        box.set_margin_top(8)
        box.set_margin_bottom(8)

        for src_path, src_type, src_data in sources:
            frame = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
            frame.set_hexpand(True)

            # Label
            size_str = f"{len(src_data)/1024:.0f}K"
            lbl = Gtk.Label(label=f"{src_path.name} ({size_str})")
            frame.append(lbl)

            # Image
            try:
                loader = GdkPixbuf.PixbufLoader()
                loader.write(src_data)
                loader.close()
                pixbuf = loader.get_pixbuf()
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                img = Gtk.Picture.new_for_paintable(texture)
                img.set_size_request(200, 200)
                img.set_content_fit(Gtk.ContentFit.CONTAIN)
                frame.append(img)
            except Exception:
                frame.append(Gtk.Label(label="(cannot preview)"))

            box.append(frame)

        win.set_child(box)
        win.present()

    def _on_do_merge(self, _btn):
        """Collect conflict resolutions and prompt for save path."""
        # Resolve conflicts from combos
        self._resolved = {}
        if self._conflicts:
            for key, (combo, sources) in self._conflict_combos.items():
                idx = int(combo.get_active_id() or "0")
                _, atype, data = sources[idx]
                self._resolved[key] = (atype, data)

        # Ask where to save
        dialog = Gtk.FileDialog()
        dialog.set_title("Save merged .slop as")
        dialog.set_initial_name("merged.slop")
        filt = Gtk.FileFilter()
        filt.set_name("SLOP archives")
        filt.add_pattern("*.slop")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(filt)
        dialog.set_filters(filters)
        if SLOP_DIR.exists():
            dialog.set_initial_folder(Gio.File.new_for_path(str(SLOP_DIR)))
        dialog.save(self, None, self._on_save_chosen)

    def _on_save_chosen(self, dialog, result):
        try:
            gfile = dialog.save_finish(result)
            out_path = gfile.get_path()
        except GLib.Error:
            return
        if not out_path:
            return
        if not out_path.endswith(".slop"):
            out_path += ".slop"
        out_path = Path(out_path)

        # Build merged dict: add all files in order, then override with conflict picks
        merged = {}
        for path, entries in self._file_entries.items():
            merged.update(entries)

        # Apply user conflict resolutions
        for key, val in self._resolved.items():
            merged[key] = val

        write_slop(out_path, merged)
        size_mb = out_path.stat().st_size / 1024 / 1024

        if self._app and hasattr(self._app, '_log'):
            conflicts_note = f", {len(self._resolved)} conflicts resolved" if self._resolved else ""
            self._app._log(f"Merged {len(self._file_entries)} files → {out_path.name}: "
                           f"{len(merged)} entries, {size_mb:.1f} MB{conflicts_note}")
            self._app._slop_entry.set_text(str(out_path))

        self.close()


# ── GTK Application ─────────────────────────────────────────────────

class DatBakeApp(Gtk.Application):
    def __init__(self, dat_dir: Path):
        super().__init__(application_id="com.mudproxy.datbake")
        self._dat_dir = dat_dir
        self._jobs: list[DatBakeJob] = []
        self._current_job_idx = 0
        self._running = False
        self._cancel = False
        self._pipe = None
        self._model_key = "flux-klein-9b"
        self._steps = 4
        self._gen_depth = False
        self._gen_3d = False
        self._skip_existing = True
        self._thread: Optional[threading.Thread] = None

        # Counts from scan
        self._n_monsters = 0
        self._n_items = 0
        self._n_rooms = 0
        self._n_unique_rooms = 0

    def do_activate(self):
        win = Gtk.ApplicationWindow(application=self)
        win.set_title("MajorMUD Dat → SLOP Baker")
        win.set_default_size(800, 600)

        css = Gtk.CssProvider()
        css.load_from_string(
            ".bake-header { background: #1a1a2e; color: #e0e0e0; padding: 12px; }"
            ".bake-log { background: #0d0d1a; color: #b0b0c0; padding: 8px; font-family: monospace; font-size: 10px; }"
            ".bake-stat { color: #8888cc; font-size: 11px; }"
            ".bake-progress { min-height: 20px; }"
            ".vram-frame { background: #111122; border-radius: 4px; padding: 4px 8px; }"
            ".vram-label { font-size: 10px; font-family: monospace; color: #b0b0c0; }"
        )
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )

        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        win.set_child(vbox)

        # Header
        header = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        header.add_css_class("bake-header")
        title = Gtk.Label(label=f"Dat Directory: {self._dat_dir}")
        title.set_halign(Gtk.Align.START)
        header.append(title)

        self._stats_label = Gtk.Label(label="Click 'Scan' to parse .dat files")
        self._stats_label.add_css_class("bake-stat")
        self._stats_label.set_halign(Gtk.Align.START)
        header.append(self._stats_label)
        vbox.append(header)

        # Controls
        controls = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        controls.set_margin_start(12)
        controls.set_margin_end(12)
        controls.set_margin_top(8)
        controls.set_margin_bottom(8)

        self._scan_btn = Gtk.Button(label="Scan")
        self._scan_btn.set_tooltip_text("Parse .dat files and build job list")
        self._scan_btn.connect("clicked", lambda _: self._do_scan())
        controls.append(self._scan_btn)

        # ── Include (what to process) ──
        inc_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        inc_lbl = Gtk.Label(label="<b>Include</b>", use_markup=True)
        inc_lbl.set_halign(Gtk.Align.START)
        inc_box.append(inc_lbl)

        self._chk_monsters = Gtk.CheckButton(label="Monsters")
        self._chk_monsters.set_active(True)
        inc_box.append(self._chk_monsters)

        self._chk_items = Gtk.CheckButton(label="Items")
        self._chk_items.set_active(True)
        inc_box.append(self._chk_items)

        self._chk_rooms = Gtk.CheckButton(label="Rooms")
        self._chk_rooms.set_active(True)
        inc_box.append(self._chk_rooms)

        controls.append(inc_box)

        # ── Phases (what work to do) ──
        phase_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        phase_lbl = Gtk.Label(label="<b>Phases</b>", use_markup=True)
        phase_lbl.set_halign(Gtk.Align.START)
        phase_box.append(phase_lbl)

        self._chk_gen_prompts = Gtk.CheckButton(label="Generate prompts (Ollama)")
        self._chk_gen_prompts.set_active(True)
        self._chk_gen_prompts.set_tooltip_text("Generate image prompts for entries missing them")
        phase_box.append(self._chk_gen_prompts)

        self._chk_gen_images = Gtk.CheckButton(label="Generate images (FLUX)")
        self._chk_gen_images.set_active(True)
        self._chk_gen_images.set_tooltip_text("Generate images for entries that have prompts but no image")
        phase_box.append(self._chk_gen_images)

        self._chk_depth = Gtk.CheckButton(label="Generate depth maps")
        self._chk_depth.set_active(False)
        phase_box.append(self._chk_depth)

        self._chk_inpaint = Gtk.CheckButton(label="Generate inpaint (LaMa)")
        self._chk_inpaint.set_active(False)
        self._chk_inpaint.set_tooltip_text("Depth-aware disocclusion inpainting for parallax — requires depth maps")
        phase_box.append(self._chk_inpaint)

        controls.append(phase_box)

        # ── Options ──
        opt_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        opt_lbl = Gtk.Label(label="<b>Options</b>", use_markup=True)
        opt_lbl.set_halign(Gtk.Align.START)
        opt_box.append(opt_lbl)

        self._chk_3d = Gtk.CheckButton(label="3D-optimized prompts")
        self._chk_3d.set_active(False)
        opt_box.append(self._chk_3d)

        self._chk_skip = Gtk.CheckButton(label="Skip existing assets")
        self._chk_skip.set_active(True)
        opt_box.append(self._chk_skip)

        self._chk_prompts = Gtk.CheckButton(label="Package prompts in .slop")
        self._chk_prompts.set_active(True)
        self._chk_prompts.set_tooltip_text("Include prompts in .slop for debugging")
        opt_box.append(self._chk_prompts)

        self._chk_autosave = Gtk.CheckButton(label="Autosave every 100 images")
        self._chk_autosave.set_active(True)
        self._chk_autosave.set_tooltip_text("Saves progress automatically so nothing is lost on crash")
        opt_box.append(self._chk_autosave)

        controls.append(opt_box)

        # Per-type model selectors
        model_grid = Gtk.Grid()
        model_grid.set_row_spacing(2)
        model_grid.set_column_spacing(6)

        def _make_model_combo(default_id="flux-klein-9b"):
            combo = Gtk.ComboBoxText()
            for key, cfg in MODELS.items():
                combo.append(key, cfg["name"])
            combo.set_active_id(default_id)
            return combo

        lbl = Gtk.Label(label="Monsters:")
        lbl.set_halign(Gtk.Align.END)
        model_grid.attach(lbl, 0, 0, 1, 1)
        self._model_monster = _make_model_combo("flux-klein-4b")
        model_grid.attach(self._model_monster, 1, 0, 1, 1)

        lbl = Gtk.Label(label="Items:")
        lbl.set_halign(Gtk.Align.END)
        model_grid.attach(lbl, 0, 1, 1, 1)
        self._model_item = _make_model_combo("flux-klein-4b")
        model_grid.attach(self._model_item, 1, 1, 1, 1)

        lbl = Gtk.Label(label="Rooms:")
        lbl.set_halign(Gtk.Align.END)
        model_grid.attach(lbl, 0, 2, 1, 1)
        self._model_room = _make_model_combo("flux-klein-9b")
        model_grid.attach(self._model_room, 1, 2, 1, 1)

        steps_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        steps_box.append(Gtk.Label(label="Steps:"))
        self._steps_spin = Gtk.SpinButton.new_with_range(1, 50, 1)
        self._steps_spin.set_value(4)
        steps_box.append(self._steps_spin)
        model_grid.attach(steps_box, 0, 3, 2, 1)

        lbl = Gtk.Label(label="Prompt LLM:")
        lbl.set_halign(Gtk.Align.END)
        model_grid.attach(lbl, 0, 4, 1, 1)
        self._ollama_combo = Gtk.ComboBoxText()
        for m in OLLAMA_MODELS:
            self._ollama_combo.append(m, m)
        self._ollama_combo.set_active_id(OLLAMA_MODEL)
        model_grid.attach(self._ollama_combo, 1, 4, 1, 1)

        self._chk_regen_prompts = Gtk.CheckButton(label="Regenerate all prompts")
        self._chk_regen_prompts.set_active(False)
        model_grid.attach(self._chk_regen_prompts, 0, 5, 2, 1)

        controls.append(model_grid)

        # Action buttons
        btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)

        self._start_btn = Gtk.Button(label="Bake")
        self._start_btn.set_tooltip_text("Start generating images")
        self._start_btn.connect("clicked", lambda _: self._do_start())
        self._start_btn.set_sensitive(False)
        btn_box.append(self._start_btn)

        self._cancel_btn = Gtk.Button(label="Stop")
        self._cancel_btn.connect("clicked", lambda _: self._do_cancel())
        self._cancel_btn.set_sensitive(False)
        btn_box.append(self._cancel_btn)

        self._save_btn = Gtk.Button(label="Save")
        self._save_btn.set_tooltip_text("Save results to .slop archive")
        self._save_btn.connect("clicked", lambda _: self._do_save())
        self._save_btn.set_sensitive(False)
        btn_box.append(self._save_btn)

        self._load_slop_btn = Gtk.Button(label="Load")
        self._load_slop_btn.set_tooltip_text("Load existing .slop to add depth maps or inspect")
        self._load_slop_btn.connect("clicked", lambda _: self._do_load_slop())
        btn_box.append(self._load_slop_btn)

        self._merge_btn = Gtk.Button(label="Merge")
        self._merge_btn.set_tooltip_text("Merge multiple .slop files into one")
        self._merge_btn.connect("clicked", lambda _: self._do_merge_slop())
        btn_box.append(self._merge_btn)

        controls.append(btn_box)

        # ── Output .slop file selector ──
        slop_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        slop_box.set_margin_start(12)
        slop_box.set_margin_end(12)
        slop_box.append(Gtk.Label(label="Output:"))
        self._slop_entry = Gtk.Entry()
        self._slop_entry.set_hexpand(True)
        self._slop_entry.set_placeholder_text("Auto-generated filename")
        slop_box.append(self._slop_entry)
        browse_btn = Gtk.Button(label="Browse...")
        browse_btn.connect("clicked", lambda _: self._do_browse_slop())
        slop_box.append(browse_btn)
        controls.append(slop_box)

        vbox.append(controls)

        # ── Style controls ──
        style_frame = Gtk.Frame(label="Style Suffixes (appended to generated prompts)")
        style_frame.set_margin_start(12)
        style_frame.set_margin_end(12)
        style_frame.set_margin_bottom(4)
        style_grid = Gtk.Grid()
        style_grid.set_row_spacing(4)
        style_grid.set_column_spacing(8)
        style_grid.set_margin_start(8)
        style_grid.set_margin_end(8)
        style_grid.set_margin_top(4)
        style_grid.set_margin_bottom(4)

        # Room style
        lbl = Gtk.Label(label="Rooms:")
        lbl.set_halign(Gtk.Align.END)
        style_grid.attach(lbl, 0, 0, 1, 1)
        self._style_room = Gtk.Entry()
        self._style_room.set_text(ROOM_STYLE_SUFFIX)
        self._style_room.set_hexpand(True)
        self._style_room.set_tooltip_text("Default room style — matches live proxy exactly")
        style_grid.attach(self._style_room, 1, 0, 1, 1)
        reset_room = Gtk.Button(label="Reset")
        reset_room.connect("clicked", lambda _: self._style_room.set_text(ROOM_STYLE_SUFFIX))
        style_grid.attach(reset_room, 2, 0, 1, 1)

        # Monster style
        lbl = Gtk.Label(label="Monsters:")
        lbl.set_halign(Gtk.Align.END)
        style_grid.attach(lbl, 0, 1, 1, 1)
        self._style_monster = Gtk.Entry()
        self._style_monster.set_text("portrait, square frame, fantasy art, detailed")
        self._style_monster.set_hexpand(True)
        self._style_monster.set_tooltip_text("Default monster/NPC style — matches live proxy exactly")
        style_grid.attach(self._style_monster, 1, 1, 1, 1)
        reset_mon = Gtk.Button(label="Reset")
        reset_mon.connect("clicked", lambda _: self._style_monster.set_text(
            "portrait, square frame, fantasy art, detailed"))
        style_grid.attach(reset_mon, 2, 1, 1, 1)

        # Item style
        lbl = Gtk.Label(label="Items:")
        lbl.set_halign(Gtk.Align.END)
        style_grid.attach(lbl, 0, 2, 1, 1)
        self._style_item = Gtk.Entry()
        self._style_item.set_text(
            "single object laid flat on dark background, "
            "no person, no mannequin, no figure, item icon, fantasy art, detailed")
        self._style_item.set_hexpand(True)
        self._style_item.set_tooltip_text("Default item style — matches live proxy exactly")
        style_grid.attach(self._style_item, 1, 2, 1, 1)
        reset_item = Gtk.Button(label="Reset")
        reset_item.connect("clicked", lambda _: self._style_item.set_text(
            "single object laid flat on dark background, "
            "no person, no mannequin, no figure, item icon, fantasy art, detailed"))
        style_grid.attach(reset_item, 2, 2, 1, 1)

        # Extra style (appended to everything)
        lbl = Gtk.Label(label="Extra:")
        lbl.set_halign(Gtk.Align.END)
        style_grid.attach(lbl, 0, 3, 1, 1)
        self._style_extra = Gtk.Entry()
        self._style_extra.set_text("")
        self._style_extra.set_placeholder_text("Optional — appended to all prompts (e.g. 'oil painting, dark palette')")
        self._style_extra.set_hexpand(True)
        style_grid.attach(self._style_extra, 1, 3, 1, 1)
        clear_extra = Gtk.Button(label="Clear")
        clear_extra.connect("clicked", lambda _: self._style_extra.set_text(""))
        style_grid.attach(clear_extra, 2, 3, 1, 1)

        style_frame.set_child(style_grid)
        vbox.append(style_frame)

        # ── VRAM monitor ──
        vram_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        vram_box.set_margin_start(12)
        vram_box.set_margin_end(12)
        vram_box.set_margin_top(2)
        vram_box.set_margin_bottom(2)
        vram_box.add_css_class("vram-frame")

        vram_icon = Gtk.Label(label="VRAM")
        vram_icon.add_css_class("vram-label")
        vram_box.append(vram_icon)

        self._vram_bar = Gtk.DrawingArea()
        self._vram_bar.set_hexpand(True)
        self._vram_bar.set_content_height(16)
        self._vram_used_frac = 0.0
        self._vram_bar.set_draw_func(self._draw_vram_bar)
        vram_box.append(self._vram_bar)

        self._vram_label = Gtk.Label(label="—")
        self._vram_label.add_css_class("vram-label")
        self._vram_label.set_width_chars(28)
        self._vram_label.set_xalign(1.0)
        vram_box.append(self._vram_label)

        vbox.append(vram_box)

        # Start VRAM polling
        self._vram_total_mb = 0
        GLib.timeout_add(1000, self._poll_vram)

        # Progress bars
        prog_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        prog_box.set_margin_start(12)
        prog_box.set_margin_end(12)
        prog_box.set_margin_top(4)
        prog_box.set_margin_bottom(4)

        # Overall progress
        overall_label = Gtk.Label(label="Overall Progress")
        overall_label.set_halign(Gtk.Align.START)
        prog_box.append(overall_label)
        self._overall_progress = Gtk.ProgressBar()
        self._overall_progress.add_css_class("bake-progress")
        prog_box.append(self._overall_progress)

        self._overall_count = Gtk.Label(label="")
        self._overall_count.set_halign(Gtk.Align.START)
        self._overall_count.add_css_class("bake-stat")
        prog_box.append(self._overall_count)

        # Phase progress
        phase_label = Gtk.Label(label="Current Phase")
        phase_label.set_halign(Gtk.Align.START)
        prog_box.append(phase_label)
        self._phase_progress = Gtk.ProgressBar()
        self._phase_progress.add_css_class("bake-progress")
        prog_box.append(self._phase_progress)

        self._phase_count = Gtk.Label(label="")
        self._phase_count.set_halign(Gtk.Align.START)
        self._phase_count.add_css_class("bake-stat")
        prog_box.append(self._phase_count)

        self._current_label = Gtk.Label(label="")
        self._current_label.set_halign(Gtk.Align.START)
        prog_box.append(self._current_label)

        self._time_label = Gtk.Label(label="")
        self._time_label.set_halign(Gtk.Align.START)
        self._time_label.add_css_class("bake-stat")
        prog_box.append(self._time_label)

        vbox.append(prog_box)

        # Log
        scroll = Gtk.ScrolledWindow()
        scroll.set_vexpand(True)
        self._log_view = Gtk.TextView()
        self._log_view.set_editable(False)
        self._log_view.add_css_class("bake-log")
        self._log_buf = self._log_view.get_buffer()
        scroll.set_child(self._log_view)
        vbox.append(scroll)

        win.present()

    def _log(self, msg: str):
        def _ui():
            end = self._log_buf.get_end_iter()
            self._log_buf.insert(end, msg + "\n")
            # Auto-scroll
            mark = self._log_buf.create_mark(None, self._log_buf.get_end_iter(), False)
            self._log_view.scroll_mark_onscreen(mark)
        GLib.idle_add(_ui)

    @staticmethod
    def _fmt_time(seconds: float) -> str:
        """Format seconds as H:MM:SS or M:SS."""
        s = int(seconds)
        if s >= 3600:
            return f"{s // 3600}:{(s % 3600) // 60:02d}:{s % 60:02d}"
        return f"{s // 60}:{s % 60:02d}"

    def _set_overall(self, done: int, total: int, phase_name: str = ""):
        elapsed = time.monotonic() - getattr(self, '_bake_start_time', time.monotonic())
        if done > 0:
            avg_per_item = elapsed / done
            remaining_items = total - done
            eta_secs = avg_per_item * remaining_items
        else:
            eta_secs = 0

        elapsed_str = self._fmt_time(elapsed)
        eta_str = self._fmt_time(eta_secs) if done > 0 else "calculating..."
        avg_str = f"{elapsed / done:.1f}s" if done > 0 else "—"

        def _ui():
            frac = done / total if total > 0 else 0
            self._overall_progress.set_fraction(max(0, min(1, frac)))
            self._overall_progress.set_text(f"{phase_name}  {done}/{total}" if phase_name else f"{done}/{total}")
            self._overall_progress.set_show_text(True)
            remaining = total - done
            self._overall_count.set_text(
                f"Done: {done} | Remaining: {remaining} | Total: {total}")
            self._time_label.set_text(
                f"Elapsed: {elapsed_str} | ETA: {eta_str} | Avg: {avg_str}/item")
        GLib.idle_add(_ui)

    def _set_phase(self, done: int, total: int, text: str = ""):
        def _ui():
            frac = done / total if total > 0 else 0
            self._phase_progress.set_fraction(max(0, min(1, frac)))
            self._phase_progress.set_text(text if text else f"{done}/{total}")
            self._phase_progress.set_show_text(True)
            remaining = total - done
            self._phase_count.set_text(
                f"Phase: {done}/{total} done, {remaining} remaining")
        GLib.idle_add(_ui)

    def _set_current(self, text: str):
        GLib.idle_add(lambda: self._current_label.set_text(text))

    # ── VRAM monitor ──

    def _draw_vram_bar(self, area, cr, width, height):
        """Draw the VRAM usage bar: green → yellow → red."""
        frac = max(0.0, min(1.0, self._vram_used_frac))

        # Background
        cr.set_source_rgb(0.08, 0.08, 0.15)
        cr.rectangle(0, 0, width, height)
        cr.fill()

        if frac <= 0:
            return

        # Color: green (0-60%) → yellow (60-80%) → red (80-100%)
        if frac < 0.6:
            r, g, b = 0.2, 0.8, 0.3
        elif frac < 0.8:
            t = (frac - 0.6) / 0.2  # 0..1 within yellow zone
            r = 0.2 + t * 0.8  # 0.2 → 1.0
            g = 0.8 - t * 0.1  # 0.8 → 0.7
            b = 0.3 - t * 0.2  # 0.3 → 0.1
        else:
            t = (frac - 0.8) / 0.2
            r = 1.0
            g = 0.7 - t * 0.5  # 0.7 → 0.2
            b = 0.1 - t * 0.05  # 0.1 → 0.05

        bar_w = int(width * frac)
        cr.set_source_rgb(r, g, b)
        cr.rectangle(0, 0, bar_w, height)
        cr.fill()

        # Subtle border
        cr.set_source_rgba(1, 1, 1, 0.1)
        cr.set_line_width(1)
        cr.rectangle(0.5, 0.5, width - 1, height - 1)
        cr.stroke()

    def _poll_vram(self) -> bool:
        """Poll GPU VRAM usage every second."""
        try:
            # Use nvidia-smi to avoid importing torch on the main thread
            import subprocess
            result = subprocess.run(
                ["nvidia-smi", "--query-gpu=memory.used,memory.total",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=2,
            )
            if result.returncode == 0:
                parts = result.stdout.strip().split(",")
                used_mb = float(parts[0].strip())
                total_mb = float(parts[1].strip())
                self._vram_total_mb = total_mb
                self._vram_used_frac = used_mb / total_mb if total_mb > 0 else 0

                # Color the label text to match bar
                frac = self._vram_used_frac
                if frac >= 0.8:
                    color = "#ff4444"
                elif frac >= 0.6:
                    color = "#ddaa22"
                else:
                    color = "#44cc55"

                self._vram_label.set_markup(
                    f'<span color="{color}">'
                    f'{used_mb:.0f} / {total_mb:.0f} MB ({frac*100:.0f}%)'
                    f'</span>'
                )
                self._vram_bar.queue_draw()
        except Exception:
            self._vram_label.set_text("no GPU")
            self._vram_used_frac = 0
            self._vram_bar.queue_draw()
        return True  # keep polling

    # ── Scan ──

    def _do_scan(self):
        self._log("Scanning .dat files...")
        self._jobs.clear()

        dat = self._dat_dir

        # Find dat files (case-insensitive)
        def find_dat(name):
            for f in dat.iterdir():
                if f.name.lower() == name.lower():
                    return f
            return None

        # Parse monsters
        mon_path = find_dat("wccknms2.dat")
        if mon_path:
            monsters = parse_monsters(mon_path)
            self._n_monsters = len(monsters)
            # Only include monsters with descriptions
            with_desc = [m for m in monsters if m["desc"]]
            self._log(f"  Monsters: {len(monsters)} total, {len(with_desc)} with descriptions")
            cached_prompts = 0
            for m in with_desc:
                job = DatBakeJob(
                    key=m["name"].lower(),
                    name=m["name"],
                    asset_type=ASSET_NPC_THUMB,
                    description=m["desc"],
                )
                cached = self._load_cached_prompt(job.key)
                if cached:
                    job.prompt = cached
                    cached_prompts += 1
                self._jobs.append(job)
            if cached_prompts:
                self._log(f"    ({cached_prompts} prompts loaded from cache)")
        else:
            self._log("  wccknms2.dat not found")

        # Parse items
        item_path = find_dat("wccitem2.dat")
        if item_path:
            items = parse_items(item_path)
            self._n_items = len(items)
            with_desc = [i for i in items if i["desc"]]
            self._log(f"  Items: {len(items)} total, {len(with_desc)} with descriptions")
            cached_prompts = 0
            for i in with_desc:
                job = DatBakeJob(
                    key=i["name"].lower(),
                    name=i["name"],
                    asset_type=ASSET_ITEM_THUMB,
                    description=i["desc"],
                )
                cached = self._load_cached_prompt(job.key)
                if cached:
                    job.prompt = cached
                    cached_prompts += 1
                self._jobs.append(job)
            if cached_prompts:
                self._log(f"    ({cached_prompts} prompts loaded from cache)")
        else:
            self._log("  wccitem2.dat not found")

        # Parse rooms — deduplicate by description
        room_path = find_dat("wccmp002.dat")
        if room_path:
            rooms = parse_rooms(room_path)
            self._n_rooms = len(rooms)
            # Deduplicate: same name + same exits = same image (matches live proxy cache key)
            seen_descs = {}
            for r in rooms:
                if not r["desc"]:
                    continue
                exits_str = '|'.join(sorted(r.get("exits", [])))
                cache_key = hashlib.sha256(
                    f"{r['name']}|{exits_str}".encode()
                ).hexdigest()[:16]
                if cache_key not in seen_descs:
                    seen_descs[cache_key] = r
            self._n_unique_rooms = len(seen_descs)
            self._log(f"  Rooms: {len(rooms)} total, {len(seen_descs)} unique (name+exits)")
            cached_prompts = 0
            for cache_key, r in seen_descs.items():
                exits = r.get("exits", [])
                exits_line = f"\nExits: {', '.join(exits)}" if exits else ""
                job_key = f"room_{cache_key}"
                job = DatBakeJob(
                    key=job_key,
                    name=f"{r['name']} ({r['key']})",
                    asset_type=ASSET_ROOM_IMAGE,
                    description=f"{r['name']}\n{r['desc']}{exits_line}",
                    room_meta={"map": r["map"], "room": r["room"],
                               "name": r["name"], "exits": exits},
                    width=768,
                    height=512,
                )
                cached = self._load_cached_prompt(job_key)
                if cached:
                    job.prompt = cached
                    cached_prompts += 1
                self._jobs.append(job)
            if cached_prompts:
                self._log(f"    ({cached_prompts} prompts loaded from cache)")
        else:
            self._log("  wccmp002.dat not found")

        # Merge pre-existing assets from loaded .slop into scanned jobs
        loaded = getattr(self, '_loaded_entries', {})
        if loaded:
            n_reused = 0
            for job in self._jobs:
                entry = loaded.get(job.key)
                if entry and entry.get("image"):
                    job.image_bytes = entry["image"]
                    job.done = True
                    if entry.get("depth"):
                        job.depth_bytes = entry["depth"]
                    if entry.get("inpaint"):
                        job.inpaint_bytes = entry["inpaint"]
                    if entry.get("prompt") and not job.prompt:
                        job.prompt = entry["prompt"]
                    n_reused += 1
            if n_reused:
                self._log(f"  Merged {n_reused} existing assets from loaded .slop")

        total = len(self._jobs)
        n_done = sum(1 for j in self._jobs if j.done)
        n_todo = total - n_done
        self._stats_label.set_text(
            f"Monsters: {self._n_monsters} | Items: {self._n_items} | "
            f"Rooms: {self._n_rooms} ({self._n_unique_rooms} unique) | "
            f"Total: {total} ({n_done} done, {n_todo} to generate)"
        )
        self._log(f"Scan complete: {total} bake jobs ({n_done} already done, {n_todo} need generation)")
        self._start_btn.set_sensitive(total > 0)

    # ── Start/Cancel/Save ──

    def _do_start(self):
        # Filter jobs based on Include checkboxes
        active_jobs = []
        if self._chk_monsters.get_active():
            active_jobs += [j for j in self._jobs if j.asset_type == ASSET_NPC_THUMB]
        if self._chk_items.get_active():
            active_jobs += [j for j in self._jobs if j.asset_type == ASSET_ITEM_THUMB]
        if self._chk_rooms.get_active():
            active_jobs += [j for j in self._jobs if j.asset_type == ASSET_ROOM_IMAGE]

        # Phase checkboxes
        self._gen_prompts = self._chk_gen_prompts.get_active()
        self._gen_images = self._chk_gen_images.get_active()
        self._gen_depth = self._chk_depth.get_active()
        self._gen_inpaint = self._chk_inpaint.get_active()
        self._gen_3d = self._chk_3d.get_active()
        self._skip_existing = self._chk_skip.get_active()
        self._steps = int(self._steps_spin.get_value())

        if not active_jobs:
            self._log("No jobs selected — check at least one Include category")
            return

        # Per-type model assignments
        model_monster = self._model_monster.get_active_id() or "flux-klein-9b"
        model_item = self._model_item.get_active_id() or "flux-klein-9b"
        model_room = self._model_room.get_active_id() or "flux-klein-9b"
        for j in active_jobs:
            if j.asset_type == ASSET_NPC_THUMB:
                j.model_key = model_monster
            elif j.asset_type == ASSET_ITEM_THUMB:
                j.model_key = model_item
            elif j.asset_type == ASSET_ROOM_IMAGE:
                j.model_key = model_room

        # Ollama model selection
        self._ollama_model = self._ollama_combo.get_active_id() or OLLAMA_MODEL

        # Regenerate all prompts — clear cached prompts to force regen
        if self._chk_regen_prompts.get_active():
            for j in active_jobs:
                j.prompt = ""

        # Snapshot style text from GUI (thread-safe)
        self._style_room_text = self._style_room.get_text().strip() or ROOM_STYLE_SUFFIX
        self._style_room_3d_text = ROOM_STYLE_SUFFIX_3D  # 3D always uses the standard 3D suffix
        self._style_monster_text = self._style_monster.get_text().strip() or "portrait, square frame, fantasy art, detailed"
        self._style_item_text = self._style_item.get_text().strip() or (
            "single object laid flat on dark background, "
            "no person, no mannequin, no figure, item icon, fantasy art, detailed")
        self._style_extra_text = self._style_extra.get_text().strip()

        self._jobs = active_jobs
        self._running = True
        self._cancel = False

        self._start_btn.set_sensitive(False)
        self._scan_btn.set_sensitive(False)
        self._cancel_btn.set_sensitive(True)
        self._save_btn.set_sensitive(False)

        self._bake_start_time = time.monotonic()
        self._log(f"Starting bake: {len(self._jobs)} jobs, steps={self._steps}")
        self._thread = threading.Thread(target=self._bake_worker, daemon=True)
        self._thread.start()

    def _do_cancel(self):
        self._cancel = True
        self._cancel_btn.set_sensitive(False)
        self._log("Cancelling...")

    def _do_save(self):
        user_path = self._slop_entry.get_text().strip()
        if user_path:
            if not user_path.endswith(".slop"):
                user_path += ".slop"
            slop_path = Path(user_path)
            slop_path.parent.mkdir(parents=True, exist_ok=True)
        else:
            SLOP_DIR.mkdir(parents=True, exist_ok=True)
            ts = time.strftime("%Y%m%d_%H%M%S")
            slop_path = SLOP_DIR / f"majormud_1.11p_{ts}.slop"

        include_prompts = self._chk_prompts.get_active()
        entries = {}
        n_prompts = 0
        for job in self._jobs:
            if not job.done or not job.image_bytes:
                continue
            entries[job.key] = (job.asset_type, job.image_bytes)
            if job.depth_bytes:
                entries[f"{job.key}_depth"] = (ASSET_DEPTH_MAP, job.depth_bytes)
            if job.inpaint_bytes:
                entries[f"{job.key}_inpaint"] = (ASSET_INPAINT, job.inpaint_bytes)
            if include_prompts and job.prompt:
                prompt_data = job.prompt.encode("utf-8")
                entries[f"{job.key}_prompt"] = (ASSET_PROMPT, prompt_data)
                n_prompts += 1
            if job.room_meta:
                entries[f"{job.key}_rmeta"] = (ASSET_METADATA, json.dumps(job.room_meta).encode("utf-8"))

        if not entries:
            self._log("No completed assets to save")
            return

        # Count done per type from all jobs (not just entries)
        done_npc = sum(1 for j in self._jobs if j.done and j.asset_type == ASSET_NPC_THUMB)
        done_item = sum(1 for j in self._jobs if j.done and j.asset_type == ASSET_ITEM_THUMB)
        done_room = sum(1 for j in self._jobs if j.done and j.asset_type == ASSET_ROOM_IMAGE)
        done_depth = sum(1 for j in self._jobs if j.depth_bytes and j.asset_type == ASSET_ROOM_IMAGE)
        done_inpaint = sum(1 for j in self._jobs if j.inpaint_bytes and j.asset_type == ASSET_ROOM_IMAGE)

        # Embed metadata — totals from scan + what's done
        metadata = {
            "source": "MajorMUD 1.11p",
            "dat_dir": str(self._dat_dir),
            "created": time.strftime("%Y-%m-%d %H:%M:%S"),
            "total_monsters": self._n_monsters,
            "total_items": self._n_items,
            "total_rooms": self._n_rooms,
            "total_unique_rooms": self._n_unique_rooms,
            "done_npc": done_npc,
            "done_item": done_item,
            "done_room": done_room,
            "done_depth": done_depth,
            "done_inpaint": done_inpaint,
        }
        entries["_metadata"] = (ASSET_METADATA, json.dumps(metadata).encode("utf-8"))

        # If appending to existing file, merge entries
        if slop_path.exists():
            try:
                existing = read_slop(slop_path)
                existing.update(entries)
                entries = existing
                self._log(f"Appending to existing {slop_path.name}")
            except Exception as e:
                self._log(f"Warning: could not read existing file: {e}")

        write_slop(slop_path, entries)
        size_mb = slop_path.stat().st_size / 1024 / 1024
        prompt_note = f" ({n_prompts} prompts)" if n_prompts else ""
        self._log(f"Saved {len(entries)} assets{prompt_note} to {slop_path} ({size_mb:.1f} MB)")
        self._slop_entry.set_text(str(slop_path))

    def _do_load_slop(self):
        """Load an existing .slop file, show contents summary, and allow depth pass."""
        dialog = Gtk.FileDialog()
        dialog.set_title("Load .slop file")
        filt = Gtk.FileFilter()
        filt.set_name("SLOP archives")
        filt.add_pattern("*.slop")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(filt)
        dialog.set_filters(filters)
        if SLOP_DIR.exists():
            dialog.set_initial_folder(Gio.File.new_for_path(str(SLOP_DIR)))
        dialog.open(self.get_active_window(), None, self._on_slop_file_chosen)

    def _on_slop_file_chosen(self, dialog, result):
        try:
            gfile = dialog.open_finish(result)
        except GLib.Error:
            return  # user cancelled
        path = Path(gfile.get_path())
        self._log(f"Loading {path.name}...")

        try:
            entries = read_slop(path)
        except Exception as e:
            self._log(f"  Failed to load: {e}")
            return

        # Extract metadata if present
        meta = {}
        if "_metadata" in entries:
            asset_type, meta_bytes = entries.pop("_metadata")
            if asset_type == ASSET_METADATA:
                try:
                    meta = json.loads(meta_bytes.decode("utf-8"))
                except Exception:
                    pass

        total_npc = meta.get("total_monsters", 0)
        total_item = meta.get("total_items", 0)
        total_room = meta.get("total_unique_rooms", 0)
        total_room_raw = meta.get("total_rooms", 0)
        source = meta.get("source", "unknown")
        created = meta.get("created", "unknown")

        # Count actual assets
        n_npc = sum(1 for k, (t, _) in entries.items() if t == ASSET_NPC_THUMB)
        n_item = sum(1 for k, (t, _) in entries.items() if t == ASSET_ITEM_THUMB)
        n_room = sum(1 for k, (t, _) in entries.items() if t == ASSET_ROOM_IMAGE)
        n_depth = sum(1 for k, (t, _) in entries.items() if t == ASSET_DEPTH_MAP)
        n_inpaint = sum(1 for k, (t, _) in entries.items() if t == ASSET_INPAINT)
        n_prompt = sum(1 for k, (t, _) in entries.items() if t == ASSET_PROMPT)

        size_mb = path.stat().st_size / 1024 / 1024
        self._log(f"  Loaded {path.name}: {size_mb:.1f} MB")
        if meta:
            self._log(f"  Source: {source} | Created: {created}")
            if total_room_raw:
                self._log(f"  Dat totals: {total_npc} monsters, {total_item} items, "
                          f"{total_room_raw} rooms ({total_room} unique)")

        def _frac(done, total):
            if total > 0:
                pct = done * 100 // total
                return f"{done}/{total} ({pct}%)"
            return f"{done}"

        self._log(f"  Contents:")
        self._log(f"    Monsters:   {_frac(n_npc, total_npc)}")
        self._log(f"    Items:      {_frac(n_item, total_item)}")
        self._log(f"    Rooms:      {_frac(n_room, total_room)}")
        self._log(f"    Depth maps: {_frac(n_depth, total_room)}")
        self._log(f"    Inpaint:    {_frac(n_inpaint, n_room)}")
        if n_prompt:
            self._log(f"    Prompts:    {n_prompt}")

        # Report what's missing
        missing = []
        if total_npc and n_npc < total_npc:
            missing.append(f"monsters {n_npc}/{total_npc}")
        if total_item and n_item < total_item:
            missing.append(f"items {n_item}/{total_item}")
        if total_room and n_room < total_room:
            missing.append(f"rooms {n_room}/{total_room}")
        if n_room > 0 and n_depth < n_room:
            missing.append(f"depth maps {n_depth}/{n_room}")
        if n_depth > 0 and n_inpaint < n_depth:
            missing.append(f"inpaint {n_inpaint}/{n_depth}")

        if missing:
            self._log(f"  Incomplete: {', '.join(missing)}")
        elif total_npc:
            self._log(f"  Slop is complete!")

        # Store loaded entries so Scan can merge pre-existing assets into new jobs
        self._loaded_entries = {}  # {key: {image, depth, inpaint, prompt, type}}
        loaded_depth_keys = {k.replace("_depth", "")
                            for k, (t, _) in entries.items() if t == ASSET_DEPTH_MAP}
        loaded_inpaint_keys = {k.replace("_inpaint", "")
                              for k, (t, _) in entries.items() if t == ASSET_INPAINT}

        # Collect all prompts first (prompt-only entries have no image)
        loaded_prompts = {}
        for key, (asset_type, data) in entries.items():
            if asset_type == ASSET_PROMPT and key.endswith("_prompt"):
                base_key = key[:-7]  # strip "_prompt"
                loaded_prompts[base_key] = data.decode("utf-8", errors="replace")

        # Collect image entries
        for key, (asset_type, data) in entries.items():
            if asset_type in (ASSET_ROOM_IMAGE, ASSET_NPC_THUMB, ASSET_ITEM_THUMB):
                entry = {"image": data, "type": asset_type}
                if key in loaded_depth_keys:
                    entry["depth"] = entries.get(f"{key}_depth", (0, None))[1]
                if key in loaded_inpaint_keys:
                    entry["inpaint"] = entries.get(f"{key}_inpaint", (0, None))[1]
                if key in loaded_prompts:
                    entry["prompt"] = loaded_prompts[key]
                self._loaded_entries[key] = entry

        # Also create entries for prompts that have NO image yet (need generation)
        for base_key, prompt_text in loaded_prompts.items():
            if base_key not in self._loaded_entries:
                # Determine type from key prefix
                if base_key.startswith("room_"):
                    atype = ASSET_ROOM_IMAGE
                else:
                    # Could be NPC or item — check if it looks like an item key
                    # Default to NPC since we can't tell without .dat context
                    atype = ASSET_NPC_THUMB
                self._loaded_entries[base_key] = {
                    "prompt": prompt_text,
                    "type": atype,
                }

        # Build jobs — both complete and incomplete
        self._jobs.clear()
        self._loaded_slop_path = path

        for key, entry in self._loaded_entries.items():
            prompt = entry.get("prompt", "")
            has_image = "image" in entry
            job = DatBakeJob(
                key=key,
                name=key,
                asset_type=entry["type"],
                description="",
                prompt=prompt,
                width=768 if entry["type"] == ASSET_ROOM_IMAGE else 512,
                height=512,
            )
            if has_image:
                job.image_bytes = entry["image"]
                job.done = True
                job.depth_bytes = entry.get("depth")
                job.inpaint_bytes = entry.get("inpaint")
            self._jobs.append(job)

        n_with_image = sum(1 for j in self._jobs if j.done)
        n_need_image = sum(1 for j in self._jobs if not j.done and j.prompt)
        n_need_prompt = sum(1 for j in self._jobs if not j.prompt)
        rooms_without_depth = sum(1 for j in self._jobs
                                  if j.asset_type == ASSET_ROOM_IMAGE and j.done and not j.depth_bytes)
        rooms_without_inpaint = sum(1 for j in self._jobs
                                    if j.asset_type == ASSET_ROOM_IMAGE and j.depth_bytes and not j.inpaint_bytes)

        self._log(f"  Jobs: {n_with_image} have images, {n_need_image} have prompts but need images, "
                  f"{n_need_prompt} need prompts")

        # Store totals from metadata for re-save
        if total_npc:
            self._n_monsters = total_npc
        if total_item:
            self._n_items = total_item
        if total_room_raw:
            self._n_rooms = total_room_raw
        if total_room:
            self._n_unique_rooms = total_room

        self._stats_label.set_text(
            f"Loaded: {_frac(n_npc, total_npc)} NPCs | {_frac(n_item, total_item)} items | "
            f"{_frac(n_room, total_room)} rooms | "
            f"{n_depth} depth | {n_inpaint} inpaint | "
            f"{n_need_image} need images"
        )

        can_work = n_need_image > 0 or rooms_without_depth > 0 or rooms_without_inpaint > 0
        self._start_btn.set_sensitive(can_work)
        self._save_btn.set_sensitive(n_with_image > 0)
        if n_need_image > 0:
            self._log(f"  Ready: {n_need_image} entries have prompts but need image generation")
        if rooms_without_depth > 0:
            self._log(f"  Ready: {rooms_without_depth} rooms need depth maps")
        if rooms_without_inpaint > 0:
            self._log(f"  Ready: {rooms_without_inpaint} rooms need inpainting")

    def _do_browse_slop(self):
        """Open file chooser for .slop output path."""
        dialog = Gtk.FileDialog()
        dialog.set_title("Choose output .slop file")
        filt = Gtk.FileFilter()
        filt.set_name("SLOP archives")
        filt.add_pattern("*.slop")
        all_filt = Gtk.FileFilter()
        all_filt.set_name("All files")
        all_filt.add_pattern("*")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(filt)
        filters.append(all_filt)
        dialog.set_filters(filters)
        if SLOP_DIR.exists():
            dialog.set_initial_folder(Gio.File.new_for_path(str(SLOP_DIR)))
        dialog.save(self.get_active_window(), None, self._on_browse_slop_done)

    def _on_browse_slop_done(self, dialog, result):
        try:
            gfile = dialog.save_finish(result)
            path = gfile.get_path()
        except GLib.Error:
            return
        if path:
            if not path.endswith(".slop"):
                path += ".slop"
            self._slop_entry.set_text(path)
            self._log(f"Output set to: {path}")

    def _do_merge_slop(self):
        """Pick multiple .slop files to merge into one."""
        dialog = Gtk.FileDialog()
        dialog.set_title("Select .slop files to merge")
        filt = Gtk.FileFilter()
        filt.set_name("SLOP archives")
        filt.add_pattern("*.slop")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(filt)
        dialog.set_filters(filters)
        if SLOP_DIR.exists():
            dialog.set_initial_folder(Gio.File.new_for_path(str(SLOP_DIR)))
        dialog.open_multiple(self.get_active_window(), None, self._on_merge_select_done)

    def _on_merge_select_done(self, dialog, result):
        try:
            gfiles = dialog.open_multiple_finish(result)
        except GLib.Error:
            return

        paths = []
        for i in range(gfiles.get_n_items()):
            gf = gfiles.get_item(i)
            p = gf.get_path()
            if p:
                paths.append(Path(p))

        if len(paths) < 2:
            self._log("Need at least 2 files to merge")
            return

        # Load all files and detect conflicts
        file_entries = {}  # path -> {key: (type, data)}
        for p in paths:
            try:
                file_entries[p] = read_slop(p)
            except Exception as e:
                self._log(f"  Error reading {p.name}: {e}")

        if len(file_entries) < 2:
            self._log("Could not load enough files")
            return

        # Show the merge preview dialog
        merge_dlg = MergeDialog(self.get_active_window(), file_entries, app=self)
        merge_dlg.present()

    # (merge dialog handles the rest — see MergeDialog class below)

    # ── Worker thread ────────────────────────────────────────────────

    @staticmethod
    def _prompt_cache_path(key: str) -> Path:
        safe = hashlib.sha256(key.encode()).hexdigest()[:16]
        return PROMPT_CACHE_DIR / f"{safe}.txt"

    def _load_cached_prompt(self, key: str) -> str | None:
        path = self._prompt_cache_path(key)
        if path.exists():
            text = path.read_text().strip()
            return text if len(text) > 20 else None
        return None

    def _save_cached_prompt(self, key: str, prompt: str):
        PROMPT_CACHE_DIR.mkdir(parents=True, exist_ok=True)
        self._prompt_cache_path(key).write_text(prompt)

    def _generate_prompt_ollama(self, job: DatBakeJob) -> str:
        """Generate image prompt from description via Ollama."""
        import urllib.request

        if job.asset_type == ASSET_ITEM_THUMB:
            system = ITEM_SYSTEM_PROMPT
            prompt_text = f"Name: {job.name}\nDescription: {job.description}"
        elif job.asset_type == ASSET_ROOM_IMAGE:
            system = ROOM_SYSTEM_PROMPT
            prompt_text = job.description
        else:
            system = PORTRAIT_SYSTEM_PROMPT
            prompt_text = f"Name: {job.name}\nDescription: {job.description}"

        payload = json.dumps({
            "model": getattr(self, '_ollama_model', OLLAMA_MODEL),
            "system": system,
            "prompt": prompt_text,
            "stream": False,
        }).encode()

        req = urllib.request.Request(
            OLLAMA_URL,
            data=payload,
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=120) as resp:
            data = json.loads(resp.read().decode())

        response = data.get("response", "").strip()
        # Strip thinking tags
        if "<think>" in response:
            parts = response.split("</think>")
            response = parts[-1].strip()
        return response.strip("\"'")

    def _build_full_prompt(self, job: DatBakeJob) -> str:
        """Add style suffix to the base prompt, using GUI style entries."""
        extra = self._style_extra_text
        if job.asset_type == ASSET_ROOM_IMAGE:
            style = self._style_room_3d_text if self._gen_3d else self._style_room_text
            prompt = f"{job.prompt}, {style}"
        elif job.asset_type == ASSET_ITEM_THUMB:
            prompt = f"{job.prompt}, {self._style_item_text}"
        else:
            prompt = f"{job.prompt}, {self._style_monster_text}"
        if extra:
            prompt = f"{prompt}, {extra}"
        return prompt

    def _bake_worker(self):
        total = len(self._jobs)
        gen_prompts = getattr(self, '_gen_prompts', True)
        gen_images = getattr(self, '_gen_images', True)

        # Count work per phase
        need_prompts = [(i, j) for i, j in enumerate(self._jobs) if not j.prompt] if gen_prompts else []
        need_images = [j for j in self._jobs if j.prompt and not j.done] if gen_images else []
        need_depth = [j for j in self._jobs if j.done and j.image_bytes
                      and j.asset_type == ASSET_ROOM_IMAGE and not j.depth_bytes] if self._gen_depth else []
        need_inpaint = [j for j in self._jobs if j.done and j.image_bytes
                        and j.asset_type == ASSET_ROOM_IMAGE and not j.inpaint_bytes] if self._gen_inpaint else []

        n_prompts = len(need_prompts)
        n_images = len(need_images)
        n_depth = len(need_depth)
        n_inpaint_est = len(need_inpaint)
        total_work = n_prompts + n_images + n_depth + n_inpaint_est
        work_done = 0

        self._log(f"  Plan: {n_prompts} prompts, {n_images} images, "
                  f"{n_depth} depth maps, {n_inpaint_est} inpaint")

        if total_work == 0:
            self._finish("Nothing to do — all phases complete or disabled")
            return

        # ── Phase 1: Generate prompts ──
        ollama_model = getattr(self, '_ollama_model', OLLAMA_MODEL)
        self._log(f"── Phase 1: Generating {n_prompts} image prompts via Ollama ({ollama_model}) ──")
        self._set_overall(0, total_work, "Phase 1: Prompts")

        # Pre-pull the model if needed (ollama pull is a no-op if already downloaded)
        if n_prompts > 0:
            self._set_current(f"Checking model: {ollama_model}...")
            try:
                import urllib.request
                pull_req = urllib.request.Request(
                    "http://localhost:11434/api/pull",
                    data=json.dumps({"name": ollama_model, "stream": False}).encode(),
                    headers={"Content-Type": "application/json"},
                )
                self._log(f"  Pulling {ollama_model} (will skip if already downloaded)...")
                urllib.request.urlopen(pull_req, timeout=600)  # 10 min for big models
                self._log(f"  Model {ollama_model} ready")
            except Exception as e:
                self._log(f"  WARNING: Could not pull model {ollama_model}: {e}")
                self._log(f"  Make sure Ollama is running: ollama pull {ollama_model}")

        consecutive_failures = 0
        MAX_CONSECUTIVE_FAILURES = 3

        for pi, (ji, job) in enumerate(need_prompts):
            if self._cancel:
                break
            self._set_current(f"LLM prompt: {job.name}")
            try:
                prompt = self._generate_prompt_ollama(job)
                ok, reason = BakeApp._validate_prompt(prompt)
                if not ok:
                    self._log(f"  [retry] {job.name}: {reason}, retrying...")
                    prompt = self._generate_prompt_ollama(job)
                    ok, reason = BakeApp._validate_prompt(prompt)

                if ok:
                    job.prompt = prompt
                    self._save_cached_prompt(job.key, prompt)
                    self._log(f"  [{pi+1}/{n_prompts}] {job.name}: {prompt[:80]}...")
                    consecutive_failures = 0
                else:
                    self._log(f"  [!] {job.name}: {reason} — \"{prompt[:60]}\"")
                    job.error = f"bad prompt: {reason}"
                    consecutive_failures += 1
            except Exception as e:
                self._log(f"  [!] LLM failed for {job.name}: {e}")
                job.error = str(e)
                consecutive_failures += 1

            if consecutive_failures >= MAX_CONSECUTIVE_FAILURES:
                self._log(f"  STOPPING: {MAX_CONSECUTIVE_FAILURES} consecutive LLM failures — "
                          f"is Ollama running? Is the model loaded?")
                break

            work_done += 1
            self._set_phase(pi + 1, n_prompts, f"Prompt {pi+1}/{n_prompts}")
            self._set_overall(work_done, total_work, "Phase 1: Prompts")

        if self._cancel:
            self._finish("Cancelled during prompt generation")
            return

        # ── Phase 2: Unload Ollama (only if we used it) ──
        if n_prompts > 0:
            self._log("── Phase 2: Freeing VRAM ──")
            self._set_current("Unloading Ollama...")
            try:
                import urllib.request
                import subprocess
                try:
                    ps_data = json.loads(urllib.request.urlopen(
                        "http://localhost:11434/api/ps", timeout=5
                    ).read().decode())
                    for m in ps_data.get("models", []):
                        model_name = m.get("name", "")
                        req = urllib.request.Request(
                            OLLAMA_URL,
                            data=json.dumps({"model": model_name, "keep_alive": 0}).encode(),
                            headers={"Content-Type": "application/json"},
                        )
                        urllib.request.urlopen(req, timeout=10)
                        self._log(f"  Unloaded model: {model_name}")
                except Exception:
                    pass
                self._log("  Stopping Ollama service...")
                subprocess.run(["systemctl", "--user", "stop", "ollama"],
                              timeout=10, capture_output=True)
                time.sleep(2)
            except Exception as e:
                self._log(f"  Warning: Could not stop Ollama: {e}")

        # Import torch for image gen / depth / inpaint
        import torch
        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()

        device = "cuda" if torch.cuda.is_available() else "cpu"
        dtype = torch.bfloat16 if device == "cuda" else torch.float32

        if self._cancel:
            self._finish("Cancelled")
            return

        # ── Phase 3: Generate images (grouped by model) ──
        if gen_images:
            ready = [(i, j) for i, j in enumerate(self._jobs)
                     if j.prompt and not j.done and not j.error]
        else:
            ready = []
        n_ready = len(ready)
        self._log(f"── Phase 3: Generating {n_ready} images ──")
        self._set_overall(work_done, total_work, "Phase 3: Images")
        generated = 0
        errors = 0

        # Group by model to minimize reloads
        from collections import OrderedDict
        model_groups: OrderedDict[str, list] = OrderedDict()
        for pi, (ji, job) in enumerate(ready):
            model_groups.setdefault(job.model_key, []).append((pi, ji, job))

        models_needed = list(model_groups.keys())
        self._log(f"  Models needed: {', '.join(MODELS[k]['name'] for k in models_needed)}")

        current_model_key = None
        pi_global = 0  # global image counter across all groups

        for model_key in models_needed:
            group = model_groups[model_key]
            model_cfg = MODELS[model_key]

            # Load model if different from current
            if model_key != current_model_key:
                if current_model_key is not None:
                    self._log(f"  Switching model: {MODELS[current_model_key]['name']} → {model_cfg['name']}")
                    self._unload_pipe()

                self._set_current(f"Loading {model_cfg['name']}...")
                self._log(f"  Loading {model_cfg['name']}...")

                # Check flash attention availability
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

                if has_flash:
                    self._log(f"  Flash Attention 2 enabled")
                else:
                    self._log(f"  Using default attention (flash-attn not installed)")

                if device == "cuda":
                    self._pipe.enable_model_cpu_offload()
                    self._log(f"  {model_cfg['name']} loaded with CPU offload")
                else:
                    self._pipe = self._pipe.to(device)
                    self._log(f"  {model_cfg['name']} loaded on {device}")

                current_model_key = model_key

            if self._cancel:
                break

            # Generate images for this model group
            n_group = len(group)
            self._log(f"  Generating {n_group} images with {model_cfg['name']}")

            for gi, (pi, ji, job) in enumerate(group):
                if self._cancel:
                    break

                self._set_current(f"[{model_cfg['name']}] {job.name} ({pi_global+1}/{n_ready})")
                full_prompt = self._build_full_prompt(job)

                try:
                    import random
                    seed = random.randint(0, 2**32 - 1)
                    gen_device = "cpu" if device == "cuda" else device
                    generator = torch.Generator(device=gen_device).manual_seed(seed)
                    steps = self._steps

                    def _step_cb(pipe, step, timestep, kwargs,
                                 _pi=pi_global, _n=n_ready, _name=job.name, _s=steps):
                        self._set_phase(
                            _pi, _n,
                            f"Image {_pi+1}/{_n}: {_name} (step {step+1}/{_s})")
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
                    self._log(f"  [{pi_global+1}/{n_ready}] {job.name} ({len(job.image_bytes)/1024:.0f} KB)")

                except Exception as e:
                    job.error = str(e)
                    errors += 1
                    self._log(f"  [!] {job.name}: {e}")

                work_done += 1
                pi_global += 1
                self._set_phase(pi_global, n_ready, f"Images: {pi_global}/{n_ready}")
                self._set_overall(work_done, total_work, "Phase 3: Images")

                # Autosave every 100 images
                if generated > 0 and generated % 100 == 0 and self._chk_autosave.get_active():
                    self._autosave(generated)

        # ── Phase 4: Depth maps (optional) ──
        if self._gen_depth and not self._cancel:
            self._unload_pipe()
            room_jobs = [j for j in self._jobs
                        if j.done and j.image_bytes and j.asset_type == ASSET_ROOM_IMAGE
                        and not j.depth_bytes]
            self._log(f"── Phase 4: Generating {len(room_jobs)} depth maps ──")
            self._set_overall(work_done, total_work, "Phase 4: Depth Maps")
            self._generate_depth_maps(room_jobs, work_done, total_work)

        # ── Phase 5: Inpainting (optional, requires depth) ──
        if self._gen_inpaint and not self._cancel:
            inpaint_jobs = [j for j in self._jobs
                           if j.done and j.image_bytes and j.depth_bytes
                           and j.asset_type == ASSET_ROOM_IMAGE and not j.inpaint_bytes]
            if inpaint_jobs:
                self._log(f"── Phase 5: Inpainting {len(inpaint_jobs)} rooms ──")
                self._generate_inpaint(inpaint_jobs, work_done, total_work)
                done_inpaint = sum(1 for j in inpaint_jobs if j.inpaint_bytes)
                self._log(f"  Inpaint: {done_inpaint}/{len(inpaint_jobs)} generated")
            else:
                self._log("── Phase 5: No rooms need inpainting (no depth maps?) ──")

        # ── Cleanup ──
        self._unload_pipe()
        status = "Cancelled" if self._cancel else "Complete"
        self._finish(f"{status}: {generated} generated, {errors} errors")

    def _generate_depth_maps(self, jobs: list[DatBakeJob],
                             work_done: int = 0, total_work: int = 0):
        """Run depth estimation on completed room images — model loaded once on GPU."""
        import torch
        import numpy as np
        from PIL import Image

        n_depth = len(jobs)
        self._log(f"  Loading Depth Anything V2 Large on GPU...")
        self._set_current("Loading depth model...")

        try:
            from transformers import pipeline as hf_pipeline
            device = "cuda" if torch.cuda.is_available() else "cpu"
            depth_pipe = hf_pipeline(
                "depth-estimation",
                model="depth-anything/Depth-Anything-V2-Large-hf",
                device=device,
            )
            self._log(f"  Depth model loaded on {device}")
        except Exception as e:
            self._log(f"  Failed to load depth model: {e}")
            return

        for pi, job in enumerate(jobs):
            if self._cancel:
                break
            self._set_current(f"Depth: {job.name} ({pi+1}/{n_depth})")
            self._set_phase(pi, n_depth, f"Depth map {pi+1}/{n_depth}")
            self._set_overall(work_done + pi, total_work, "Depth Maps")

            try:
                img = Image.open(io.BytesIO(job.image_bytes)).convert("RGB")
                result = depth_pipe(img)
                depth_pil = result["depth"]
                depth = np.array(depth_pil, dtype=np.float32)

                # Normalize to 8-bit
                d_min, d_max = depth.min(), depth.max()
                if d_max - d_min > 1e-8:
                    depth_norm = (depth - d_min) / (d_max - d_min)
                else:
                    depth_norm = np.zeros_like(depth)
                depth_8bit = (depth_norm * 255).astype(np.uint8)

                # Encode as PNG
                buf = io.BytesIO()
                Image.fromarray(depth_8bit, mode="L").save(buf, format="PNG")
                job.depth_bytes = buf.getvalue()
                self._log(f"  [{pi+1}/{n_depth}] {job.name} depth OK ({len(job.depth_bytes)//1024}K)")

            except Exception as e:
                self._log(f"  [!] Depth error for {job.name}: {e}")

        # Unload depth model
        del depth_pipe
        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()
        self._log(f"  Depth model unloaded")

    def _generate_inpaint(self, jobs: list[DatBakeJob],
                          work_done: int = 0, total_work: int = 0):
        """Run LaMa depth-aware disocclusion inpainting on rooms with depth maps."""
        import torch
        import numpy as np
        import cv2
        from PIL import Image

        n_inpaint = len(jobs)
        self._log(f"  Loading SimpleLama on GPU...")
        self._set_current("Loading inpaint model...")

        try:
            from simple_lama_inpainting import SimpleLama
            device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
            lama = SimpleLama(device=device)
            self._log(f"  LaMa loaded on {device}")
        except Exception as e:
            self._log(f"  Failed to load LaMa: {e}")
            return

        DILATION = 20
        THRESHOLD = 25

        for pi, job in enumerate(jobs):
            if self._cancel:
                break
            self._set_current(f"Inpaint: {job.name} ({pi+1}/{n_inpaint})")
            self._set_phase(pi, n_inpaint, f"Inpaint {pi+1}/{n_inpaint}")
            self._set_overall(work_done + pi, total_work, "Inpaint")

            if not job.image_bytes or not job.depth_bytes:
                continue

            try:
                image = Image.open(io.BytesIO(job.image_bytes)).convert("RGB")
                depth = Image.open(io.BytesIO(job.depth_bytes)).convert("L")

                # Resize depth to match image if needed
                if depth.size != image.size:
                    depth = depth.resize(image.size, Image.BILINEAR)

                depth_np = np.array(depth, dtype=np.float32)

                # Skip flat depth maps
                if depth_np.max() - depth_np.min() < 2.0:
                    # No meaningful depth — just copy image as inpaint
                    buf = io.BytesIO()
                    image.save(buf, format="WebP", quality=90)
                    job.inpaint_bytes = buf.getvalue()
                    self._log(f"  [{pi+1}/{n_inpaint}] {job.name} flat depth, copied original")
                    continue

                depth_u8 = np.array(depth, dtype=np.uint8)

                # Sobel edge detection on depth
                sobel_x = cv2.Sobel(depth_u8, cv2.CV_64F, 1, 0, ksize=3)
                sobel_y = cv2.Sobel(depth_u8, cv2.CV_64F, 0, 1, ksize=3)
                gradient_mag = np.sqrt(sobel_x ** 2 + sobel_y ** 2)

                grad_max = gradient_mag.max()
                if grad_max < 1e-8:
                    buf = io.BytesIO()
                    image.save(buf, format="WebP", quality=90)
                    job.inpaint_bytes = buf.getvalue()
                    continue

                gradient_norm = (gradient_mag / grad_max * 255.0).astype(np.uint8)

                # Threshold to binary edge mask
                _, edge_mask = cv2.threshold(gradient_norm, THRESHOLD, 255, cv2.THRESH_BINARY)

                # Dilate edges
                kernel_size = DILATION * 2 + 1
                kernel = cv2.getStructuringElement(
                    cv2.MORPH_ELLIPSE, (kernel_size, kernel_size))
                dilated_edges = cv2.dilate(edge_mask, kernel, iterations=1)

                # Background side: depth below local blurred depth
                depth_blurred = cv2.GaussianBlur(depth_np, (0, 0), sigmaX=DILATION)
                background_side = depth_np < depth_blurred

                # Disocclusion mask: dilated edge region AND background side
                disocclusion_mask = np.zeros_like(edge_mask)
                disocclusion_mask[(dilated_edges > 0) & background_side] = 255

                # Thin edge strip to avoid seams
                small_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
                thin_edge = cv2.dilate(edge_mask, small_kernel, iterations=1)
                disocclusion_mask[thin_edge > 0] = 255

                if disocclusion_mask.max() == 0:
                    buf = io.BytesIO()
                    image.save(buf, format="WebP", quality=90)
                    job.inpaint_bytes = buf.getvalue()
                    continue

                # Run LaMa inpainting
                mask_pil = Image.fromarray(disocclusion_mask, mode="L")
                result = lama(image, mask_pil)

                buf = io.BytesIO()
                result.save(buf, format="WebP", quality=90)
                job.inpaint_bytes = buf.getvalue()
                self._log(f"  [{pi+1}/{n_inpaint}] {job.name} inpaint OK ({len(job.inpaint_bytes)//1024}K)")

            except Exception as e:
                self._log(f"  [!] Inpaint error for {job.name}: {e}")

        # Unload LaMa
        del lama
        gc.collect()
        import torch as _torch
        if _torch.cuda.is_available():
            _torch.cuda.empty_cache()
        self._log(f"  LaMa unloaded")

    def _autosave(self, count: int):
        """Autosave current progress to .slop file."""
        user_path = None
        try:
            user_path = self._slop_entry.get_text().strip()
        except Exception:
            pass

        if user_path:
            if not user_path.endswith(".slop"):
                user_path += ".slop"
            slop_path = Path(user_path)
        elif hasattr(self, '_loaded_slop_path'):
            slop_path = self._loaded_slop_path
        else:
            SLOP_DIR.mkdir(parents=True, exist_ok=True)
            slop_path = SLOP_DIR / "autosave.slop"

        slop_path.parent.mkdir(parents=True, exist_ok=True)

        entries = {}
        for job in self._jobs:
            if job.done and job.image_bytes:
                entries[job.key] = (job.asset_type, job.image_bytes)
                if job.depth_bytes:
                    entries[f"{job.key}_depth"] = (ASSET_DEPTH_MAP, job.depth_bytes)
                if job.inpaint_bytes:
                    entries[f"{job.key}_inpaint"] = (ASSET_INPAINT, job.inpaint_bytes)
                if job.prompt:
                    entries[f"{job.key}_prompt"] = (ASSET_PROMPT, job.prompt.encode("utf-8"))
                if job.room_meta:
                    entries[f"{job.key}_rmeta"] = (ASSET_METADATA, json.dumps(job.room_meta).encode("utf-8"))
            elif job.prompt:
                # Save prompt-only entries too so we don't lose them
                entries[f"{job.key}_prompt"] = (ASSET_PROMPT, job.prompt.encode("utf-8"))
                if job.room_meta:
                    entries[f"{job.key}_rmeta"] = (ASSET_METADATA, json.dumps(job.room_meta).encode("utf-8"))

        if not entries:
            return

        try:
            write_slop(slop_path, entries)
            size_mb = slop_path.stat().st_size / 1024 / 1024
            self._log(f"  💾 Autosaved: {count} images → {slop_path.name} ({size_mb:.1f} MB)")
        except Exception as e:
            self._log(f"  [!] Autosave failed: {e}")

    def _unload_pipe(self):
        if self._pipe is not None:
            import torch
            del self._pipe
            self._pipe = None
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
        # Restart Ollama so it's available again
        try:
            import subprocess
            subprocess.run(["systemctl", "--user", "start", "ollama"], timeout=10,
                          capture_output=True)
            self._log("  Restarted Ollama service")
        except Exception:
            pass

    def _finish(self, msg: str):
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

    if len(sys.argv) < 2:
        print("Usage: python -m mudproxy.dat_bake /path/to/dat/files/")
        print("  Directory should contain wccknms2.dat, wccitem2.dat, wccmp002.dat, etc.")
        sys.exit(1)

    dat_dir = Path(sys.argv[1])
    if not dat_dir.is_dir():
        print(f"Error: {dat_dir} is not a directory")
        sys.exit(1)

    app = DatBakeApp(dat_dir)
    app.run([sys.argv[0]])


if __name__ == "__main__":
    main()
