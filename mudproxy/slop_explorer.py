"""Slop Explorer — browse, preview, and regenerate assets in a loaded .slop file.

Integrated as a sub-module of the dat baker. Uses the baker's loaded slop data,
model settings, and generation pipeline.
"""

import gc
import io
import random
import threading
import time
from pathlib import Path

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import Gtk, Gdk, GdkPixbuf, GLib, Pango

from .bake import (
    MODELS, ASSET_NPC_THUMB, ASSET_ITEM_THUMB, ASSET_ROOM_IMAGE,
    ASSET_DEPTH_MAP, ASSET_PROMPT, ASSET_INPAINT, write_slop,
)

THUMB_SIZE = 128
PAGE_SIZE = 100  # load this many thumbnails at a time
MISSING_COLOR = (0.15, 0.15, 0.22)


# ── Thumbnail widget ─────────────────────────────────────────────

class ThumbWidget(Gtk.Box):
    """Single thumbnail cell: image + label, with hover-zoom and right-click."""

    def __init__(self, job, has_image: bool, explorer):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        self.job = job
        self.has_image = has_image
        self._explorer = explorer

        self.set_size_request(THUMB_SIZE + 16, THUMB_SIZE + 40)
        self.set_halign(Gtk.Align.CENTER)
        self.set_valign(Gtk.Align.START)

        # Image area
        self._picture = Gtk.Picture()
        self._picture.set_size_request(THUMB_SIZE, THUMB_SIZE)
        self._picture.set_content_fit(Gtk.ContentFit.CONTAIN)

        if has_image and job.image_bytes:
            self._set_image(job.image_bytes)
        else:
            # Missing placeholder
            self._picture.add_css_class("slop-missing")

        self.append(self._picture)

        # Label: ID number centered at bottom
        display = self._make_label()
        lbl = Gtk.Label(label=display)
        lbl.set_ellipsize(Pango.EllipsizeMode.END)
        lbl.set_max_width_chars(18)
        lbl.set_lines(2)
        lbl.set_halign(Gtk.Align.CENTER)
        lbl.add_css_class("slop-thumb-label")
        if not has_image:
            lbl.add_css_class("slop-missing-label")
        self.append(lbl)

        # Hover controller for zoom preview
        hover = Gtk.EventControllerMotion()
        hover.connect("enter", self._on_hover_enter)
        hover.connect("leave", self._on_hover_leave)
        self.add_controller(hover)

        # Right-click for context menu
        click = Gtk.GestureClick(button=3)
        click.connect("pressed", self._on_right_click)
        self.add_controller(click)

        # Left double-click to open regen directly
        dbl = Gtk.GestureClick(button=1)
        dbl.connect("pressed", self._on_left_click)
        self.add_controller(dbl)

    def _make_label(self) -> str:
        """Build display label from job metadata."""
        job = self.job
        if job.asset_type == ASSET_ROOM_IMAGE:
            meta = job.room_meta or {}
            m = meta.get("map", "?")
            r = meta.get("room", "?")
            name = meta.get("name", "")
            return f"M{m} R{r}\n{name}" if name else f"M{m} R{r}"
        else:
            # Show full "#42 Iron Sword"
            return job.name

    def _set_image(self, data: bytes):
        """Load image bytes into the picture widget, scaled to thumb size."""
        try:
            loader = GdkPixbuf.PixbufLoader()
            loader.write(data)
            loader.close()
            pixbuf = loader.get_pixbuf()
            if pixbuf:
                w, h = pixbuf.get_width(), pixbuf.get_height()
                scale = min(THUMB_SIZE / w, THUMB_SIZE / h, 1.0)
                if scale < 1.0:
                    pixbuf = pixbuf.scale_simple(
                        int(w * scale), int(h * scale),
                        GdkPixbuf.InterpType.BILINEAR)
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                self._picture.set_paintable(texture)
        except Exception:
            self._picture.add_css_class("slop-missing")

    def update_image(self, data: bytes):
        """Update after regeneration."""
        self.has_image = True
        self._picture.remove_css_class("slop-missing")
        self._set_image(data)

    def _on_hover_enter(self, _ctrl, _x, _y):
        if self.has_image and self.job.image_bytes:
            self._explorer.show_zoom(self, self.job)

    def _on_hover_leave(self, _ctrl):
        self._explorer.hide_zoom()

    def _on_right_click(self, gesture, _n, x, y):
        self._explorer.show_context_menu(self, x, y)

    def _on_left_click(self, gesture, n_press, _x, _y):
        if n_press == 2:
            self._explorer.open_regen_dialog(self)


# ── Regen Dialog ─────────────────────────────────────────────────

class RegenDialog(Gtk.Window):
    """Side-by-side OLD/NEW comparison with prompt editing and regeneration."""

    def __init__(self, explorer, thumb_widget):
        job = thumb_widget.job
        super().__init__(
            title=f"Regenerate: {job.name}",
            transient_for=explorer,
            modal=True,
        )
        self.set_default_size(800, 550)
        self._explorer = explorer
        self._thumb = thumb_widget
        self._job = job
        self._new_bytes = None
        self._generating = False

        self._build_ui()

    def _build_ui(self):
        job = self._job
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        vbox.set_margin_start(12)
        vbox.set_margin_end(12)
        vbox.set_margin_top(12)
        vbox.set_margin_bottom(12)
        self.set_child(vbox)

        # Title
        title = Gtk.Label(label=f"<b>{job.name}</b>", use_markup=True)
        title.set_halign(Gtk.Align.START)
        vbox.append(title)

        # Prompt editor
        prompt_lbl = Gtk.Label(label="Prompt:")
        prompt_lbl.set_halign(Gtk.Align.START)
        vbox.append(prompt_lbl)

        prompt_scroll = Gtk.ScrolledWindow()
        prompt_scroll.set_min_content_height(60)
        prompt_scroll.set_max_content_height(80)
        self._prompt_view = Gtk.TextView()
        self._prompt_view.set_wrap_mode(Gtk.WrapMode.WORD)
        self._prompt_view.add_css_class("slop-prompt-edit")
        buf = self._prompt_view.get_buffer()
        buf.set_text(job.prompt or "(no prompt — will generate one)")
        prompt_scroll.set_child(self._prompt_view)
        vbox.append(prompt_scroll)

        # Extra fix terms
        extra_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        extra_lbl = Gtk.Label(label="Fix terms:")
        extra_row.append(extra_lbl)
        self._extra_entry = Gtk.Entry()
        self._extra_entry.set_hexpand(True)
        self._extra_entry.set_placeholder_text("Optional — appended to prompt (e.g. 'brighter colors, no text')")
        extra_row.append(self._extra_entry)
        vbox.append(extra_row)

        # OLD / NEW comparison
        compare = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        compare.set_vexpand(True)

        # OLD
        old_frame = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        old_frame.set_hexpand(True)
        old_lbl = Gtk.Label(label="<b>OLD (current)</b>", use_markup=True)
        old_frame.append(old_lbl)
        self._old_picture = Gtk.Picture()
        self._old_picture.set_content_fit(Gtk.ContentFit.CONTAIN)
        self._old_picture.set_vexpand(True)
        if job.image_bytes:
            self._load_picture(self._old_picture, job.image_bytes)
        else:
            old_frame.append(Gtk.Label(label="(no existing image)"))
        old_frame.append(self._old_picture)
        compare.append(old_frame)

        # NEW
        new_frame = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        new_frame.set_hexpand(True)
        new_lbl = Gtk.Label(label="<b>NEW (regenerated)</b>", use_markup=True)
        new_frame.append(new_lbl)
        self._new_picture = Gtk.Picture()
        self._new_picture.set_content_fit(Gtk.ContentFit.CONTAIN)
        self._new_picture.set_vexpand(True)
        new_frame.append(self._new_picture)
        self._new_status = Gtk.Label(label="Click 'Generate' to create a new image")
        self._new_status.add_css_class("slop-regen-status")
        new_frame.append(self._new_status)
        compare.append(new_frame)

        vbox.append(compare)

        # Buttons
        btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        btn_box.set_halign(Gtk.Align.END)
        btn_box.set_margin_top(8)

        self._gen_btn = Gtk.Button(label="Generate")
        self._gen_btn.add_css_class("suggested-action")
        self._gen_btn.connect("clicked", self._on_generate)
        btn_box.append(self._gen_btn)

        self._keep_btn = Gtk.Button(label="Keep New")
        self._keep_btn.set_sensitive(False)
        self._keep_btn.add_css_class("bake-btn-primary")
        self._keep_btn.add_css_class("bake-action-btn")
        self._keep_btn.connect("clicked", self._on_keep)
        btn_box.append(self._keep_btn)

        cancel_btn = Gtk.Button(label="Close")
        cancel_btn.connect("clicked", lambda _: self.close())
        btn_box.append(cancel_btn)

        vbox.append(btn_box)

    def _load_picture(self, picture, data: bytes):
        try:
            loader = GdkPixbuf.PixbufLoader()
            loader.write(data)
            loader.close()
            pixbuf = loader.get_pixbuf()
            if pixbuf:
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                picture.set_paintable(texture)
        except Exception:
            pass

    def _on_generate(self, _btn):
        if self._generating:
            return
        self._generating = True
        self._gen_btn.set_sensitive(False)
        self._keep_btn.set_sensitive(False)
        self._new_status.set_text("Generating...")

        # Get prompt from editor
        buf = self._prompt_view.get_buffer()
        start, end = buf.get_bounds()
        prompt = buf.get_text(start, end, False).strip()
        extra = self._extra_entry.get_text().strip()

        thread = threading.Thread(
            target=self._generate_worker, args=(prompt, extra), daemon=True)
        thread.start()

    def _generate_worker(self, prompt: str, extra: str):
        """Background thread: generate a single image using baker's pipeline."""
        try:
            baker = self._explorer._baker
            job = self._job

            # Generate prompt if empty
            if not prompt or prompt.startswith("(no prompt"):
                GLib.idle_add(lambda: self._new_status.set_text("Generating prompt via Ollama..."))
                prompt = baker._generate_prompt_ollama(job)
                # Update the prompt view on UI thread
                def _set_prompt():
                    buf = self._prompt_view.get_buffer()
                    buf.set_text(prompt)
                GLib.idle_add(_set_prompt)

            # Build full prompt with style suffix
            job.prompt = prompt
            full_prompt = baker._build_full_prompt(job)
            if extra:
                full_prompt = f"{full_prompt}, {extra}"

            GLib.idle_add(lambda: self._new_status.set_text("Loading model..."))

            import torch

            # Get model for this asset type
            if job.asset_type == ASSET_NPC_THUMB:
                model_key = baker._model_monster.get_active_id() or "flux-klein-9b"
            elif job.asset_type == ASSET_ITEM_THUMB:
                model_key = baker._model_item.get_active_id() or "flux-klein-9b"
            else:
                model_key = baker._model_room.get_active_id() or "flux-klein-9b"

            model_cfg = MODELS[model_key]
            steps = int(baker._steps_spin.get_value())
            device = "cuda" if torch.cuda.is_available() else "cpu"
            dtype = torch.bfloat16 if device == "cuda" else torch.float32

            # Load pipeline (reuse baker's if same model, otherwise load fresh)
            pipe = getattr(baker, '_pipe', None)
            needs_load = pipe is None

            if needs_load:
                GLib.idle_add(lambda: self._new_status.set_text(
                    f"Loading {model_cfg['name']}..."))

                pipe_type = model_cfg.get("pipeline", "flux")
                if pipe_type == "sd15-lcm":
                    from diffusers import StableDiffusionPipeline, LCMScheduler
                    pipe = StableDiffusionPipeline.from_pretrained(
                        model_cfg["repo"], torch_dtype=dtype,
                        safety_checker=None).to(device)
                    pipe.scheduler = LCMScheduler.from_config(pipe.scheduler.config)
                    lora_id = model_cfg.get("lora", "latent-consistency/lcm-lora-sdv1-5")
                    pipe.load_lora_weights(lora_id)
                    pipe.fuse_lora()
                else:
                    try:
                        from diffusers import FluxPipeline
                        pipe = FluxPipeline.from_pretrained(
                            model_cfg["repo"], torch_dtype=dtype)
                    except Exception:
                        from diffusers import DiffusionPipeline
                        pipe = DiffusionPipeline.from_pretrained(
                            model_cfg["repo"], torch_dtype=dtype)
                    if device == "cuda":
                        pipe.enable_model_cpu_offload()
                    else:
                        pipe = pipe.to(device)

                baker._pipe = pipe

            GLib.idle_add(lambda: self._new_status.set_text(
                f"Generating with {model_cfg['name']} ({steps} steps)..."))

            seed = random.randint(0, 2**32 - 1)
            gen_device = "cpu" if device == "cuda" else device
            generator = torch.Generator(device=gen_device).manual_seed(seed)

            gen_width = model_cfg.get("width", job.width)
            gen_height = model_cfg.get("height", job.height)
            pipe_type = model_cfg.get("pipeline", "flux")

            with torch.no_grad():
                kwargs = dict(
                    prompt=full_prompt,
                    width=gen_width,
                    height=gen_height,
                    num_inference_steps=steps,
                    guidance_scale=model_cfg["guidance"],
                    generator=generator,
                )
                if pipe_type == "sd15-lcm":
                    kwargs["negative_prompt"] = model_cfg.get("negative", "")
                image = pipe(**kwargs).images[0]

            buf = io.BytesIO()
            image.save(buf, format="WebP", quality=90)
            self._new_bytes = buf.getvalue()

            def _show_result():
                self._load_picture(self._new_picture, self._new_bytes)
                size_kb = len(self._new_bytes) / 1024
                self._new_status.set_text(
                    f"Generated ({size_kb:.0f} KB, seed={seed})")
                self._gen_btn.set_sensitive(True)
                self._keep_btn.set_sensitive(True)
                self._generating = False
            GLib.idle_add(_show_result)

        except Exception as e:
            def _show_error():
                self._new_status.set_text(f"Error: {e}")
                self._gen_btn.set_sensitive(True)
                self._generating = False
            GLib.idle_add(_show_error)

    def _on_keep(self, _btn):
        """Replace the old image with the new one."""
        if not self._new_bytes:
            return
        job = self._job
        job.image_bytes = self._new_bytes
        job.done = True
        # Clear depth/inpaint since image changed
        job.depth_bytes = None
        job.inpaint_bytes = None

        # Update the thumbnail in the grid
        self._thumb.update_image(self._new_bytes)

        # Update loaded_entries in baker
        baker = self._explorer._baker
        if hasattr(baker, '_loaded_entries'):
            entry = baker._loaded_entries.get(job.key, {})
            entry["image"] = self._new_bytes
            entry["type"] = job.asset_type
            entry.pop("depth", None)
            entry.pop("inpaint", None)
            baker._loaded_entries[job.key] = entry

        # Mark explorer as dirty (needs save)
        self._explorer._dirty = True
        self._explorer._update_status()

        self._new_status.set_text("Kept! Image replaced.")
        self._keep_btn.set_sensitive(False)


# ── Zoom Popup ───────────────────────────────────────────────────

class ZoomPopup(Gtk.Window):
    """Floating window that shows full-size image on hover."""

    def __init__(self, parent):
        super().__init__(transient_for=parent)
        self.set_decorated(False)
        self.set_resizable(False)
        self._picture = Gtk.Picture()
        self._picture.set_content_fit(Gtk.ContentFit.CONTAIN)
        self.set_child(self._picture)

    def show_image(self, data: bytes, asset_type: int, near_widget: Gtk.Widget):
        """Display image data near the given widget."""
        try:
            loader = GdkPixbuf.PixbufLoader()
            loader.write(data)
            loader.close()
            pixbuf = loader.get_pixbuf()
            if not pixbuf:
                return

            w, h = pixbuf.get_width(), pixbuf.get_height()

            # Cap zoom size based on type
            if asset_type == ASSET_ROOM_IMAGE:
                max_w, max_h = 768, 512
            else:
                max_w, max_h = 512, 512

            scale = min(max_w / w, max_h / h, 1.0)
            display_w = int(w * scale)
            display_h = int(h * scale)

            if scale < 1.0:
                pixbuf = pixbuf.scale_simple(
                    display_w, display_h, GdkPixbuf.InterpType.BILINEAR)

            texture = Gdk.Texture.new_for_pixbuf(pixbuf)
            self._picture.set_paintable(texture)
            self._picture.set_size_request(display_w, display_h)
            self.set_default_size(display_w, display_h)
            self.present()

        except Exception:
            pass


# ── Main Explorer Window ─────────────────────────────────────────

class SlopExplorer(Gtk.Window):
    """Browse all assets in a loaded .slop, preview thumbnails, regenerate bad ones."""

    def __init__(self, baker):
        super().__init__(title="Slop Explorer")
        self.set_default_size(900, 700)
        self._baker = baker
        self._dirty = False
        self._zoom = None
        self._current_category = "monsters"
        self._thumbs: list[ThumbWidget] = []

        self._build_ui()
        self._apply_css()
        self._populate()

    def _apply_css(self):
        css = Gtk.CssProvider()
        css.load_from_string(
            ".slop-missing { background: #1a1a2e; border: 1px dashed #444466; "
            "  border-radius: 4px; }"
            ".slop-missing-label { color: #666688; font-style: italic; }"
            ".slop-thumb-label { font-size: 10px; color: #b0b0c0; }"
            ".slop-status { font-size: 11px; color: #8888cc; padding: 4px 8px; }"
            ".slop-prompt-edit { font-family: monospace; font-size: 11px; "
            "  background: #0d0d1a; color: #b0b0c0; }"
            ".slop-regen-status { font-size: 11px; color: #888; font-style: italic; }"
            ".slop-filter { padding: 4px 8px; background: #151520; }"
        )
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 1,
        )

    def _build_ui(self):
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.set_child(vbox)

        # ── Top bar: category + filters ──
        top = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        top.add_css_class("slop-filter")
        top.set_margin_start(8)
        top.set_margin_end(8)
        top.set_margin_top(4)
        top.set_margin_bottom(4)

        top.append(Gtk.Label(label="Category:"))
        self._category_combo = Gtk.ComboBoxText()
        self._category_combo.append("monsters", "Monsters")
        self._category_combo.append("items", "Items")
        self._category_combo.append("rooms", "Rooms")
        self._category_combo.set_active_id("monsters")
        self._category_combo.connect("changed", self._on_category_changed)
        top.append(self._category_combo)

        top.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))

        # Room filters (hidden for non-room categories)
        self._room_filter_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)

        self._room_filter_box.append(Gtk.Label(label="Map:"))
        self._map_spin = Gtk.SpinButton.new_with_range(0, 999, 1)
        self._map_spin.set_value(1)
        self._room_filter_box.append(self._map_spin)

        self._room_filter_box.append(Gtk.Label(label="Room:"))
        self._room_from = Gtk.SpinButton.new_with_range(0, 99999, 1)
        self._room_from.set_value(1)
        self._room_filter_box.append(self._room_from)

        self._room_filter_box.append(Gtk.Label(label="to"))
        self._room_to = Gtk.SpinButton.new_with_range(0, 99999, 1)
        self._room_to.set_value(99999)
        self._room_filter_box.append(self._room_to)

        filter_btn = Gtk.Button(label="Filter")
        filter_btn.connect("clicked", lambda _: self._populate())
        self._room_filter_box.append(filter_btn)

        self._room_filter_box.set_visible(False)
        top.append(self._room_filter_box)

        top.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))

        # Search box
        top.append(Gtk.Label(label="Search:"))
        self._search_entry = Gtk.SearchEntry()
        self._search_entry.set_hexpand(True)
        self._search_entry.set_placeholder_text("Filter by name...")
        self._search_entry.connect("search-changed", self._on_search_changed)
        top.append(self._search_entry)

        # Spacer
        spacer = Gtk.Box()
        spacer.set_hexpand(False)
        top.append(spacer)

        # Save button
        self._save_btn = Gtk.Button(label="Save Slop")
        self._save_btn.set_sensitive(False)
        self._save_btn.set_tooltip_text("Save changes back to .slop file")
        self._save_btn.connect("clicked", self._on_save)
        top.append(self._save_btn)

        vbox.append(top)

        # ── Status bar + progress ──
        self._status = Gtk.Label(label="")
        self._status.add_css_class("slop-status")
        self._status.set_halign(Gtk.Align.START)
        vbox.append(self._status)

        self._progress = Gtk.ProgressBar()
        self._progress.set_margin_start(8)
        self._progress.set_margin_end(8)
        self._progress.set_show_text(True)
        self._progress.set_visible(False)
        vbox.append(self._progress)

        # ── Thumbnail grid (FlowBox in ScrolledWindow) ──
        scroll = Gtk.ScrolledWindow()
        scroll.set_vexpand(True)
        scroll.set_hexpand(True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)

        self._flow = Gtk.FlowBox()
        self._flow.set_valign(Gtk.Align.START)
        self._flow.set_homogeneous(True)
        self._flow.set_min_children_per_line(3)
        self._flow.set_max_children_per_line(20)
        self._flow.set_column_spacing(6)
        self._flow.set_row_spacing(6)
        self._flow.set_margin_start(8)
        self._flow.set_margin_end(8)
        self._flow.set_margin_top(8)
        self._flow.set_margin_bottom(8)
        self._flow.set_selection_mode(Gtk.SelectionMode.NONE)

        scroll.set_child(self._flow)
        vbox.append(scroll)
        self._scroll = scroll

        # Load More button (shown at bottom when more pages available)
        self._load_more_btn = Gtk.Button(label="Load More...")
        self._load_more_btn.connect("clicked", lambda _: self._load_next_page())
        self._load_more_btn.set_halign(Gtk.Align.CENTER)
        self._load_more_btn.set_margin_top(8)
        self._load_more_btn.set_margin_bottom(8)
        self._load_more_btn.set_visible(False)
        vbox.append(self._load_more_btn)

    def _on_category_changed(self, combo):
        cat = combo.get_active_id()
        self._current_category = cat
        self._room_filter_box.set_visible(cat == "rooms")
        self._search_entry.set_text("")
        self._populate()

    def _on_search_changed(self, entry):
        self._populate()

    def _get_filtered_jobs(self) -> list:
        """Build the full filtered+sorted job list for current category/filters/search."""
        jobs = self._baker._jobs
        if not jobs:
            return []

        cat = self._current_category
        search = self._search_entry.get_text().strip().lower()

        def _has_content(j):
            return j.description or (j.done and j.image_bytes)

        if cat == "monsters":
            filtered = [j for j in jobs if j.asset_type == ASSET_NPC_THUMB and _has_content(j)]
            filtered.sort(key=lambda j: self._extract_num(j))
        elif cat == "items":
            filtered = [j for j in jobs if j.asset_type == ASSET_ITEM_THUMB and _has_content(j)]
            filtered.sort(key=lambda j: self._extract_num(j))
        elif cat == "rooms":
            filtered = [j for j in jobs if j.asset_type == ASSET_ROOM_IMAGE and _has_content(j)]
            map_num = int(self._map_spin.get_value())
            room_from = int(self._room_from.get_value())
            room_to = int(self._room_to.get_value())

            def _room_match(j):
                meta = j.room_meta or {}
                m = meta.get("map", 0)
                r = meta.get("room", 0)
                if map_num > 0 and m != map_num:
                    return False
                return room_from <= r <= room_to
            filtered = [j for j in filtered if _room_match(j)]
            filtered.sort(key=lambda j: (
                (j.room_meta or {}).get("map", 0),
                (j.room_meta or {}).get("room", 0),
            ))
        else:
            filtered = []

        # Apply name search
        if search:
            filtered = [j for j in filtered if search in j.name.lower()
                        or (j.room_meta and search in j.room_meta.get("name", "").lower())]

        return filtered

    def _populate(self):
        """Rebuild the thumbnail grid — loads first page only."""
        # Cancel any in-progress loading
        self._loading = False
        self._progress.set_visible(False)

        # Clear existing
        while True:
            child = self._flow.get_first_child()
            if child is None:
                break
            self._flow.remove(child)
        self._thumbs.clear()

        self._filtered = self._get_filtered_jobs()
        self._page_loaded = 0  # how many loaded so far

        if not self._filtered and not self._baker._jobs:
            self._status.set_text("No data loaded — load a .slop file in the baker first")
            self._load_more_btn.set_visible(False)
            return

        # Load first page
        self._load_next_page()

    def _load_next_page(self):
        """Append the next PAGE_SIZE thumbnails incrementally via idle callbacks."""
        if getattr(self, '_loading', False):
            return
        self._loading = True
        self._load_more_btn.set_sensitive(False)

        start = self._page_loaded
        end = min(start + PAGE_SIZE, len(self._filtered))
        total_this_page = end - start

        if total_this_page <= 0:
            self._loading = False
            return

        self._progress.set_visible(True)
        self._progress.set_fraction(0.0)
        self._progress.set_text(f"Loading 0/{total_this_page}...")

        self._load_idx = start
        self._load_end = end
        self._load_page_total = total_this_page
        self._load_page_start = start

        # Load in small batches of 5 per idle tick to keep UI responsive
        GLib.idle_add(self._load_batch)

    def _load_batch(self) -> bool:
        """Load a small batch of thumbnails per idle tick."""
        BATCH = 5
        end = min(self._load_idx + BATCH, self._load_end)

        for i in range(self._load_idx, end):
            job = self._filtered[i]
            has_img = bool(job.done and job.image_bytes)
            thumb = ThumbWidget(job, has_img, self)
            self._thumbs.append(thumb)
            self._flow.append(thumb)

        self._load_idx = end
        done_count = end - self._load_page_start
        frac = done_count / self._load_page_total if self._load_page_total else 1.0
        self._progress.set_fraction(frac)
        self._progress.set_text(f"Loading {done_count}/{self._load_page_total}...")

        if end >= self._load_end:
            # Page done
            self._page_loaded = self._load_end
            self._progress.set_visible(False)
            self._loading = False
            self._load_more_btn.set_sensitive(True)

            remaining = len(self._filtered) - self._page_loaded
            self._load_more_btn.set_visible(remaining > 0)
            if remaining > 0:
                self._load_more_btn.set_label(f"Load More ({remaining} remaining)...")

            self._update_status_text()
            return False  # stop idle

        return True  # continue next batch

    @staticmethod
    def _extract_num(job) -> int:
        """Extract ID number from job name like '#42 Iron Sword'."""
        name = job.name
        if name.startswith("#"):
            try:
                return int(name.split()[0][1:])
            except (ValueError, IndexError):
                pass
        return 0

    def _update_status_text(self):
        """Refresh status bar text."""
        filtered = getattr(self, '_filtered', [])
        cat = self._current_category
        n_total = len(filtered)
        n_with = sum(1 for j in filtered if j.done and j.image_bytes)
        n_missing = n_total - n_with
        loaded = getattr(self, '_page_loaded', 0)
        page_note = f" (showing {loaded})" if loaded < n_total else ""
        self._status.set_text(
            f"{cat.title()}: {n_total} total, {n_with} with images, "
            f"{n_missing} missing{page_note}"
            + (" — unsaved changes!" if self._dirty else "")
        )
        self._save_btn.set_sensitive(self._dirty)

    def _update_status(self):
        """Alias for after regen updates."""
        self._update_status_text()

    # ── Zoom preview ──

    def show_zoom(self, thumb: ThumbWidget, job):
        if self._zoom is None:
            self._zoom = ZoomPopup(self)
        self._zoom.show_image(job.image_bytes, job.asset_type, thumb)

    def hide_zoom(self):
        if self._zoom:
            self._zoom.hide()

    # ── Context menu ──

    def show_context_menu(self, thumb: ThumbWidget, x: float, y: float):
        menu = Gtk.PopoverMenu()
        menu_model = GLib.Menu()
        # We'll use actions instead of simple menu items

        popover = Gtk.Popover()
        popover.set_parent(thumb)
        popover.set_has_arrow(False)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        box.set_margin_start(4)
        box.set_margin_end(4)
        box.set_margin_top(4)
        box.set_margin_bottom(4)

        regen_btn = Gtk.Button(label="Regenerate...")
        regen_btn.set_has_frame(False)
        regen_btn.connect("clicked", lambda _: (popover.popdown(), self.open_regen_dialog(thumb)))
        box.append(regen_btn)

        if thumb.job.prompt:
            prompt_btn = Gtk.Button(label="View Prompt")
            prompt_btn.set_has_frame(False)
            prompt_btn.connect("clicked", lambda _: (popover.popdown(), self._show_prompt(thumb.job)))
            box.append(prompt_btn)

        if thumb.has_image:
            view_btn = Gtk.Button(label="View Full Size")
            view_btn.set_has_frame(False)
            view_btn.connect("clicked", lambda _: (popover.popdown(), self._view_full(thumb.job)))
            box.append(view_btn)

        popover.set_child(box)
        popover.popup()

    def _show_prompt(self, job):
        """Show prompt in a small dialog."""
        win = Gtk.Window(title=f"Prompt: {job.name}", transient_for=self, modal=True)
        win.set_default_size(500, 200)
        scroll = Gtk.ScrolledWindow()
        tv = Gtk.TextView()
        tv.set_editable(False)
        tv.set_wrap_mode(Gtk.WrapMode.WORD)
        tv.add_css_class("slop-prompt-edit")
        tv.get_buffer().set_text(job.prompt or "(no prompt)")
        scroll.set_child(tv)
        win.set_child(scroll)
        win.present()

    def _view_full(self, job):
        """Show full-size image in a new window."""
        if not job.image_bytes:
            return
        win = Gtk.Window(title=f"{job.name}", transient_for=self)
        try:
            loader = GdkPixbuf.PixbufLoader()
            loader.write(job.image_bytes)
            loader.close()
            pixbuf = loader.get_pixbuf()
            if pixbuf:
                w, h = pixbuf.get_width(), pixbuf.get_height()
                win.set_default_size(min(w, 1200), min(h, 800))
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                pic = Gtk.Picture.new_for_paintable(texture)
                pic.set_content_fit(Gtk.ContentFit.CONTAIN)
                win.set_child(pic)
        except Exception:
            win.set_child(Gtk.Label(label="Failed to load image"))
        win.present()

    # ── Regen dialog ──

    def open_regen_dialog(self, thumb: ThumbWidget):
        self.hide_zoom()
        dialog = RegenDialog(self, thumb)
        dialog.present()

    # ── Save ──

    def _on_save(self, _btn):
        """Save all changes back to the slop file using baker's save logic."""
        baker = self._baker
        baker._do_save()
        self._dirty = False
        self._update_status()
