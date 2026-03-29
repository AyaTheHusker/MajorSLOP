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
    MODELS, ASSET_NPC_THUMB, ASSET_ITEM_THUMB, ASSET_PLAYER_THUMB, ASSET_ROOM_IMAGE,
    ASSET_DEPTH_MAP, ASSET_PROMPT, ASSET_INPAINT, write_slop,
)
from .dat_bake import DatBakeJob

# Portrait prompt builder — reuse the curated race/class lore from gen scripts
try:
    import sys as _sys
    _scripts = str(Path(__file__).parent.parent / "scripts")
    if _scripts not in _sys.path:
        _sys.path.insert(0, _scripts)
    from gen_portrait_prompts import RACES, CLASSES, GENDERS, build_prompt as _build_portrait_prompt
    _HAS_PORTRAIT_PROMPTS = True
except ImportError:
    _HAS_PORTRAIT_PROMPTS = False

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
        elif job.asset_type == ASSET_PLAYER_THUMB:
            # "portrait_dark_elf_bard_female" → "Dark Elf\nBard Female"
            parts = job.key.replace("portrait_", "").split("_")
            if len(parts) >= 3:
                # Find gender (last part), class (second to last), race (rest)
                gender = parts[-1].title()
                cls = parts[-2].title()
                race = " ".join(p.title() for p in parts[:-2])
                return f"{race}\n{cls} {gender}"
            return job.name
        else:
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

    def _default_prompt(self):
        """Build default prompt for this job based on type."""
        job = self._job
        if job.prompt:
            return job.prompt
        if job.asset_type == ASSET_PLAYER_THUMB:
            parts = job.key.replace("portrait_", "").split("_")
            if len(parts) >= 3:
                gender = parts[-1]
                cls = parts[-2]
                race_key = " ".join(parts[:-2])
                if _HAS_PORTRAIT_PROMPTS:
                    race_match = None
                    for rname, rdata in RACES.items():
                        if rname.lower().replace("-", " ").replace("_", " ") == race_key.lower():
                            race_match = (rname, rdata)
                            break
                    cls_match = None
                    for cname, cdata in CLASSES.items():
                        if cname.lower() == cls.lower():
                            cls_match = (cname, cdata)
                            break
                    if race_match and cls_match:
                        return _build_portrait_prompt(
                            race_match[0], race_match[1],
                            cls_match[0], cls_match[1], gender)
                return (f"{gender} {race_key} {cls}, fantasy character portrait, "
                        f"head and shoulders, detailed face, dramatic lighting")
            job.width = 432
            job.height = 768
        return ""

    def _build_ui(self):
        job = self._job
        is_room = job.asset_type == ASSET_ROOM_IMAGE

        if job.asset_type == ASSET_PLAYER_THUMB:
            job.width = 432
            job.height = 768
        elif is_room:
            job.width = 768
            job.height = 512

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

        # Pipeline status indicators (for rooms)
        if is_room:
            self.set_default_size(900, 650)
            status_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
            status_row.set_margin_top(4)
            status_row.set_margin_bottom(4)

            self._indicator_image = self._make_indicator("Image", bool(job.image_bytes))
            self._indicator_upscale = self._make_indicator("Upscaled", self._is_upscaled(job))
            self._indicator_depth = self._make_indicator("Depth", bool(job.depth_bytes))
            self._indicator_inpaint = self._make_indicator("Inpaint", bool(job.inpaint_bytes))

            status_row.append(self._indicator_image[0])
            status_row.append(self._indicator_upscale[0])
            status_row.append(self._indicator_depth[0])
            status_row.append(self._indicator_inpaint[0])
            vbox.append(status_row)

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
        buf.set_text(self._default_prompt() or "(no prompt — will generate one)")
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

        # Depth + Inpaint button (rooms only, enabled after Keep New)
        if is_room:
            self._depth_btn = Gtk.Button(label="Depth + Inpaint")
            self._depth_btn.set_tooltip_text("Run Depth Anything V2 + LaMa inpainting on the new image")
            self._depth_btn.set_sensitive(False)
            self._depth_btn.connect("clicked", self._on_depth_inpaint)
            btn_box.append(self._depth_btn)

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

    @staticmethod
    def _make_indicator(label: str, done: bool) -> tuple:
        """Create a status indicator: green checkmark or red X with label."""
        box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        icon_label = Gtk.Label()
        if done:
            icon_label.set_markup('<span foreground="#44cc44">✓</span>')
        else:
            icon_label.set_markup('<span foreground="#cc4444">✗</span>')
        box.append(icon_label)
        text_label = Gtk.Label(label=label)
        text_label.add_css_class("slop-regen-status")
        box.append(text_label)
        return (box, icon_label)

    def _update_indicator(self, indicator: tuple, done: bool):
        _, icon_label = indicator
        if done:
            icon_label.set_markup('<span foreground="#44cc44">✓</span>')
        else:
            icon_label.set_markup('<span foreground="#cc4444">✗</span>')

    @staticmethod
    def _is_upscaled(job) -> bool:
        """Check if a room image is already upscaled (>768 wide)."""
        if not job.image_bytes:
            return False
        try:
            loader = GdkPixbuf.PixbufLoader()
            loader.write(job.image_bytes)
            loader.close()
            pixbuf = loader.get_pixbuf()
            return pixbuf and pixbuf.get_width() > 1024
        except Exception:
            return False

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
            elif job.asset_type == ASSET_PLAYER_THUMB:
                model_key = baker._model_monster.get_active_id() or "flux-klein-9b"
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

            # Upscale: 4x for rooms, 2x for portraits
            if job.asset_type == ASSET_ROOM_IMAGE:
                GLib.idle_add(lambda: self._new_status.set_text("Upscaling 4x with RealESRGAN..."))
                image = self._upscale_esrgan(image, scale=4)
            elif job.asset_type == ASSET_PLAYER_THUMB:
                GLib.idle_add(lambda: self._new_status.set_text("Upscaling 2x with RealESRGAN..."))
                image = self._upscale_esrgan(image, scale=2)

            buf = io.BytesIO()
            image.save(buf, format="WebP", quality=90)
            self._new_bytes = buf.getvalue()

            def _show_result():
                self._load_picture(self._new_picture, self._new_bytes)
                size_kb = len(self._new_bytes) / 1024
                w, h = image.size
                self._new_status.set_text(
                    f"Generated {w}x{h} ({size_kb:.0f} KB, seed={seed})")
                self._gen_btn.set_sensitive(True)
                self._keep_btn.set_sensitive(True)
                self._generating = False
            GLib.idle_add(_show_result)

        except Exception as e:
            err_msg = str(e)
            def _show_error():
                self._new_status.set_text(f"Error: {err_msg}")
                self._gen_btn.set_sensitive(True)
                self._generating = False
            GLib.idle_add(_show_error)

    @staticmethod
    def _upscale_esrgan(image, scale=2):
        """Upscale a PIL image using RealESRGAN via spandrel (tiled, fp16).

        scale=2 uses RealESRGAN_x2plus (portraits: 432x768 → 864x1536)
        scale=4 uses RealESRGAN_x4plus (rooms: 768x512 → 3072x2048)
        """
        import numpy as np
        import torch
        from PIL import Image
        import spandrel

        model_name = f"RealESRGAN_x{scale}plus.pth"

        # Find model weights
        model_path = None
        for candidate in [
            Path.home() / ".cache" / "mudproxy" / "models" / model_name,
            Path(__file__).parent.parent / "models" / model_name,
            Path(__file__).parent.parent / "dist" / "models" / model_name,
        ]:
            if candidate.exists():
                model_path = candidate
                break

        if model_path is None:
            print(f"{model_name} not found — skipping upscale")
            return image

        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        use_half = torch.cuda.is_available()
        model_desc = spandrel.ModelLoader().load_from_file(str(model_path))
        model = model_desc.eval().to(device)
        if use_half:
            model = model.half()
        scale = model_desc.scale

        # PIL → tensor [1, 3, H, W] float32 0-1
        w, h = image.size
        arr = np.array(image.convert("RGB"), dtype=np.float32) / 255.0
        tensor = torch.from_numpy(arr).permute(2, 0, 1).unsqueeze(0).to(device)
        if use_half:
            tensor = tensor.half()

        # Tiled inference (same approach as upscale_portraits.py)
        TILE = 512
        TILE_PAD = 10
        oh, ow = h * scale, w * scale
        out_tensor = torch.zeros((1, 3, oh, ow), dtype=tensor.dtype, device=device)

        tiles_x = max(1, (w + TILE - 1) // TILE)
        tiles_y = max(1, (h + TILE - 1) // TILE)

        for ty in range(tiles_y):
            for tx in range(tiles_x):
                x0 = tx * TILE
                y0 = ty * TILE
                x1 = min(x0 + TILE, w)
                y1 = min(y0 + TILE, h)

                px0 = max(x0 - TILE_PAD, 0)
                py0 = max(y0 - TILE_PAD, 0)
                px1 = min(x1 + TILE_PAD, w)
                py1 = min(y1 + TILE_PAD, h)

                tile_in = tensor[:, :, py0:py1, px0:px1]
                with torch.no_grad():
                    tile_out = model(tile_in)

                ox0 = (x0 - px0) * scale
                oy0 = (y0 - py0) * scale
                ox1 = ox0 + (x1 - x0) * scale
                oy1 = oy0 + (y1 - y0) * scale

                out_tensor[:, :, y0*scale:y1*scale, x0*scale:x1*scale] = \
                    tile_out[:, :, oy0:oy1, ox0:ox1]

        out_np = (out_tensor.squeeze(0).permute(1, 2, 0).float().clamp(0, 1).cpu().numpy() * 255).astype(np.uint8)
        result = Image.fromarray(out_np)

        # Cleanup
        del model, model_desc, tensor, out_tensor
        if torch.cuda.is_available():
            torch.cuda.empty_cache()
        gc.collect()

        return result

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

        # Update indicators and enable depth/inpaint for rooms
        if job.asset_type == ASSET_ROOM_IMAGE:
            self._update_indicator(self._indicator_image, True)
            self._update_indicator(self._indicator_upscale, True)
            self._update_indicator(self._indicator_depth, False)
            self._update_indicator(self._indicator_inpaint, False)
            self._depth_btn.set_sensitive(True)

    def _on_depth_inpaint(self, _btn):
        """Run Depth Anything V2 + LaMa inpaint on the current room image."""
        if self._generating:
            return
        self._generating = True
        self._depth_btn.set_sensitive(False)
        self._gen_btn.set_sensitive(False)
        self._new_status.set_text("Running depth estimation...")

        thread = threading.Thread(target=self._depth_inpaint_worker, daemon=True)
        thread.start()

    def _depth_inpaint_worker(self):
        """Background: depth map + inpaint for a room."""
        job = self._job
        try:
            import torch
            import numpy as np
            from PIL import Image

            # ── Phase 1: Depth Anything V2 ──
            GLib.idle_add(lambda: self._new_status.set_text("Loading Depth Anything V2..."))

            from transformers import pipeline as hf_pipeline
            device = "cuda" if torch.cuda.is_available() else "cpu"
            depth_pipe = hf_pipeline(
                "depth-estimation",
                model="depth-anything/Depth-Anything-V2-Large-hf",
                device=device,
            )

            GLib.idle_add(lambda: self._new_status.set_text("Generating depth map..."))
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

            buf = io.BytesIO()
            Image.fromarray(depth_8bit, mode="L").save(buf, format="PNG")
            job.depth_bytes = buf.getvalue()

            # Unload depth model
            del depth_pipe
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()

            def _depth_done():
                self._update_indicator(self._indicator_depth, True)
                self._new_status.set_text("Depth done. Running inpaint...")
            GLib.idle_add(_depth_done)

            # ── Phase 2: LaMa Inpaint ──
            import cv2
            GLib.idle_add(lambda: self._new_status.set_text("Loading LaMa inpaint model..."))

            from simple_lama_inpainting import SimpleLama
            lama_device = torch.device(device)
            lama = SimpleLama(device=lama_device)

            GLib.idle_add(lambda: self._new_status.set_text("Generating inpaint mask..."))

            depth_img = Image.open(io.BytesIO(job.depth_bytes)).convert("L")
            if depth_img.size != img.size:
                depth_img = depth_img.resize(img.size, Image.BILINEAR)

            depth_np = np.array(depth_img, dtype=np.float32)

            # Skip flat depth maps
            if depth_np.max() - depth_np.min() < 2.0:
                buf = io.BytesIO()
                img.save(buf, format="WebP", quality=90)
                job.inpaint_bytes = buf.getvalue()
            else:
                depth_u8 = np.array(depth_img, dtype=np.uint8)
                DILATION = 20
                THRESHOLD = 25

                # Sobel edge detection on depth
                sobel_x = cv2.Sobel(depth_u8, cv2.CV_64F, 1, 0, ksize=3)
                sobel_y = cv2.Sobel(depth_u8, cv2.CV_64F, 0, 1, ksize=3)
                gradient_mag = np.sqrt(sobel_x ** 2 + sobel_y ** 2)

                grad_max = gradient_mag.max()
                if grad_max < 1e-8:
                    buf = io.BytesIO()
                    img.save(buf, format="WebP", quality=90)
                    job.inpaint_bytes = buf.getvalue()
                else:
                    gradient_norm = (gradient_mag / grad_max * 255.0).astype(np.uint8)
                    _, edge_mask = cv2.threshold(gradient_norm, THRESHOLD, 255, cv2.THRESH_BINARY)

                    kernel_size = DILATION * 2 + 1
                    kernel = cv2.getStructuringElement(
                        cv2.MORPH_ELLIPSE, (kernel_size, kernel_size))
                    dilated_edges = cv2.dilate(edge_mask, kernel, iterations=1)

                    depth_blurred = cv2.GaussianBlur(depth_np, (0, 0), sigmaX=DILATION)
                    background_side = depth_np < depth_blurred

                    disocclusion_mask = np.zeros_like(edge_mask)
                    disocclusion_mask[(dilated_edges > 0) & background_side] = 255

                    # Thin edge strip
                    thin_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
                    edge_strip = cv2.dilate(edge_mask, thin_kernel, iterations=2)
                    final_mask = cv2.bitwise_or(disocclusion_mask, edge_strip)

                    GLib.idle_add(lambda: self._new_status.set_text("Inpainting..."))
                    mask_pil = Image.fromarray(final_mask, mode="L")
                    inpainted = lama(img, mask_pil)

                    buf = io.BytesIO()
                    inpainted.save(buf, format="WebP", quality=90)
                    job.inpaint_bytes = buf.getvalue()

            # Unload lama
            del lama
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()

            # Update loaded_entries
            baker = self._explorer._baker
            if hasattr(baker, '_loaded_entries'):
                entry = baker._loaded_entries.get(job.key, {})
                entry["depth"] = job.depth_bytes
                entry["inpaint"] = job.inpaint_bytes
                baker._loaded_entries[job.key] = entry

            self._explorer._dirty = True

            def _all_done():
                self._update_indicator(self._indicator_inpaint, True)
                self._new_status.set_text("Depth + Inpaint complete! All green.")
                self._generating = False
                self._gen_btn.set_sensitive(True)
                self._explorer._update_status()
            GLib.idle_add(_all_done)

        except Exception as e:
            err_msg = str(e)
            def _show_error():
                self._new_status.set_text(f"Depth/Inpaint error: {err_msg}")
                self._generating = False
                self._gen_btn.set_sensitive(True)
                self._depth_btn.set_sensitive(True)
            GLib.idle_add(_show_error)


# ── Zoom Popup ───────────────────────────────────────────────────


# ── Main Explorer Window ─────────────────────────────────────────

class SlopExplorer(Gtk.Window):
    """Browse all assets in a loaded .slop, preview thumbnails, regenerate bad ones."""

    def __init__(self, baker):
        super().__init__(title="Slop Explorer")
        self.set_default_size(1200, 750)
        self._baker = baker
        self._dirty = False
        self._current_category = "monsters"
        self._thumbs: list[ThumbWidget] = []

        self._ensure_jobs_from_loaded()
        self._build_ui()
        self._apply_css()
        self._populate()

    def _ensure_jobs_from_loaded(self):
        """Create jobs from _loaded_entries for anything not already in _jobs.

        This ensures the explorer works even without .dat files — it builds
        browseable jobs directly from whatever is in the loaded slop.
        """
        loaded = getattr(self._baker, '_loaded_entries', {})
        if not loaded:
            return

        existing_keys = {j.key for j in self._baker._jobs}

        TYPE_DIMS = {
            ASSET_NPC_THUMB: (512, 512),
            ASSET_ITEM_THUMB: (512, 512),
            ASSET_PLAYER_THUMB: (432, 768),
            ASSET_ROOM_IMAGE: (768, 512),
        }

        added = 0
        for key, entry in loaded.items():
            if key in existing_keys:
                continue
            asset_type = entry.get("type")
            if asset_type not in TYPE_DIMS:
                continue

            w, h = TYPE_DIMS[asset_type]

            # Build display name from key
            if asset_type == ASSET_PLAYER_THUMB:
                parts = key.replace("portrait_", "").split("_")
                if len(parts) >= 3:
                    gender = parts[-1].title()
                    cls = parts[-2].title()
                    race = " ".join(p.title() for p in parts[:-2])
                    name = f"{race} {cls} {gender}"
                else:
                    name = key
            else:
                # Clean up key for display: "iron_sword" → "Iron Sword"
                name = key.replace("_", " ").title()

            job = DatBakeJob(
                key=key, name=name,
                asset_type=asset_type,
                description=name,
                width=w, height=h,
            )
            if entry.get("image"):
                job.image_bytes = entry["image"]
                job.done = True
            if entry.get("depth"):
                job.depth_bytes = entry["depth"]
            if entry.get("inpaint"):
                job.inpaint_bytes = entry["inpaint"]
            if entry.get("prompt"):
                job.prompt = entry["prompt"]

            self._baker._jobs.append(job)
            added += 1

        if added:
            print(f"Explorer: added {added} jobs from loaded slop")

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
            ".slop-preview-name { font-size: 13px; color: #c0c0e0; }"
            ".slop-preview-info { font-size: 10px; color: #7777aa; }"
            ".slop-load-splash { background: rgba(10,10,20,0.85); border-radius: 12px; "
            "  padding: 24px 36px; border: 1px solid #333355; }"
            ".slop-load-splash-text { font-size: 16px; color: #aaaadd; font-weight: bold; }"
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
        self._category_combo.append("portraits", "Portraits")
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

        # (progress bar replaced by splash overlay)

        # ── Main content: thumbnails left, preview panel right ──
        paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        paned.set_vexpand(True)
        paned.set_hexpand(True)
        paned.set_shrink_start_child(False)
        paned.set_resize_start_child(True)
        paned.set_shrink_end_child(False)
        paned.set_resize_end_child(False)

        # Left: thumbnail grid
        left_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)

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
        left_box.append(scroll)
        self._scroll = scroll

        # (load more button removed — all thumbs load at once with splash)
        # Loading overlay (centered over the thumbnail grid)
        self._load_overlay = Gtk.Overlay()
        self._load_overlay.set_child(left_box)

        self._load_splash = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        self._load_splash.set_halign(Gtk.Align.CENTER)
        self._load_splash.set_valign(Gtk.Align.CENTER)
        self._load_splash.add_css_class("slop-load-splash")
        self._load_splash_label = Gtk.Label(label="Loading Monsters...")
        self._load_splash_label.add_css_class("slop-load-splash-text")
        self._load_splash.append(self._load_splash_label)
        self._load_splash_bar = Gtk.ProgressBar()
        self._load_splash_bar.set_size_request(260, -1)
        self._load_splash_bar.set_show_text(True)
        self._load_splash.append(self._load_splash_bar)
        self._load_splash.set_visible(False)
        self._load_overlay.add_overlay(self._load_splash)

        paned.set_start_child(self._load_overlay)

        # Right: preview panel (zoomed image + prompt + info)
        preview_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        preview_box.set_size_request(320, -1)
        preview_box.set_margin_start(8)
        preview_box.set_margin_end(8)
        preview_box.set_margin_top(8)
        preview_box.set_margin_bottom(8)

        # Preview title
        self._preview_name = Gtk.Label(label="")
        self._preview_name.set_halign(Gtk.Align.START)
        self._preview_name.set_wrap(True)
        self._preview_name.add_css_class("slop-preview-name")
        preview_box.append(self._preview_name)

        # Zoomed image
        preview_scroll = Gtk.ScrolledWindow()
        preview_scroll.set_vexpand(True)
        preview_scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self._preview_picture = Gtk.Picture()
        self._preview_picture.set_content_fit(Gtk.ContentFit.CONTAIN)
        self._preview_picture.set_halign(Gtk.Align.CENTER)
        self._preview_picture.set_valign(Gtk.Align.START)
        preview_scroll.set_child(self._preview_picture)
        preview_box.append(preview_scroll)

        # Image dimensions
        self._preview_info = Gtk.Label(label="")
        self._preview_info.set_halign(Gtk.Align.START)
        self._preview_info.add_css_class("slop-preview-info")
        preview_box.append(self._preview_info)

        # Prompt text (read-only, scrollable)
        prompt_lbl = Gtk.Label(label="Prompt:")
        prompt_lbl.set_halign(Gtk.Align.START)
        prompt_lbl.add_css_class("slop-preview-info")
        preview_box.append(prompt_lbl)

        prompt_scroll = Gtk.ScrolledWindow()
        prompt_scroll.set_min_content_height(80)
        prompt_scroll.set_max_content_height(150)
        self._preview_prompt = Gtk.TextView()
        self._preview_prompt.set_editable(False)
        self._preview_prompt.set_wrap_mode(Gtk.WrapMode.WORD)
        self._preview_prompt.add_css_class("slop-prompt-edit")
        prompt_scroll.set_child(self._preview_prompt)
        preview_box.append(prompt_scroll)

        paned.set_end_child(preview_box)
        paned.set_position(580)

        vbox.append(paned)

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
        elif cat == "portraits":
            filtered = [j for j in jobs if j.asset_type == ASSET_PLAYER_THUMB and _has_content(j)]
            filtered.sort(key=lambda j: j.key)
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
        """Rebuild the thumbnail grid — loads all with a splash overlay."""
        # Cancel any in-progress loading
        self._loading = False

        # Clear existing
        while True:
            child = self._flow.get_first_child()
            if child is None:
                break
            self._flow.remove(child)
        self._thumbs.clear()

        self._filtered = self._get_filtered_jobs()

        if not self._filtered and not self._baker._jobs:
            self._status.set_text("No data loaded — load a .slop file in the baker first")
            self._load_splash.set_visible(False)
            return

        if not self._filtered:
            self._update_status_text()
            self._load_splash.set_visible(False)
            return

        # Show splash overlay
        cat_names = {
            "monsters": "Monsters", "items": "Items",
            "rooms": "Rooms", "portraits": "Portraits",
        }
        cat_label = cat_names.get(self._current_category, "Assets")
        total = len(self._filtered)
        self._load_splash_label.set_text(f"Loading {cat_label}...")
        self._load_splash_bar.set_fraction(0.0)
        self._load_splash_bar.set_text(f"0 / {total}")
        self._load_splash.set_visible(True)

        self._loading = True
        self._load_idx = 0
        self._load_total = total
        GLib.idle_add(self._load_batch)

    def _load_batch(self) -> bool:
        """Load a batch of thumbnails per idle tick."""
        if not self._loading:
            return False

        BATCH = 10
        end = min(self._load_idx + BATCH, self._load_total)

        for i in range(self._load_idx, end):
            job = self._filtered[i]
            has_img = bool(job.done and job.image_bytes)
            thumb = ThumbWidget(job, has_img, self)
            self._thumbs.append(thumb)
            self._flow.append(thumb)

        self._load_idx = end
        frac = end / self._load_total
        self._load_splash_bar.set_fraction(frac)
        self._load_splash_bar.set_text(f"{end} / {self._load_total}")
        self._load_splash_label.set_text(
            f"Loading {end} / {self._load_total}  ({frac:.0%})")

        if end >= self._load_total:
            self._loading = False
            self._load_splash.set_visible(False)
            self._update_status_text()
            return False

        return True

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

    # ── Preview panel ──

    def show_zoom(self, thumb: ThumbWidget, job):
        """Update the right-side preview panel with this job's image and info."""
        if not job.image_bytes:
            return
        try:
            loader = GdkPixbuf.PixbufLoader()
            loader.write(job.image_bytes)
            loader.close()
            pixbuf = loader.get_pixbuf()
            if pixbuf:
                w, h = pixbuf.get_width(), pixbuf.get_height()
                # Scale to fit panel width (~300px) while preserving aspect
                max_w = 300
                scale = min(max_w / w, 1.0)
                display_w = int(w * scale)
                display_h = int(h * scale)
                if scale < 1.0:
                    pixbuf = pixbuf.scale_simple(
                        display_w, display_h, GdkPixbuf.InterpType.BILINEAR)
                texture = Gdk.Texture.new_for_pixbuf(pixbuf)
                self._preview_picture.set_paintable(texture)
                self._preview_picture.set_size_request(display_w, display_h)
                self._preview_name.set_markup(f"<b>{GLib.markup_escape_text(job.name)}</b>")
                self._preview_info.set_text(f"{w}×{h}  ({len(job.image_bytes) / 1024:.0f} KB)")
                prompt_text = job.prompt or "(no prompt stored)"
                self._preview_prompt.get_buffer().set_text(prompt_text)
        except Exception:
            pass

    def hide_zoom(self):
        pass  # Keep the preview showing — it's a side panel, not a popup

    # ── Context menu ──

    def show_context_menu(self, thumb: ThumbWidget, x: float, y: float):
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
