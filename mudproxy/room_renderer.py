"""Room image generator using Flux Klein 4B (or SDXL Lightning fallback).

Keeps the pipeline loaded in VRAM for fast generation.
Caches images by room name + monsters to avoid regenerating visited rooms.
Supports cancellation when the player moves mid-generation.
"""
import asyncio
import hashlib
import json
import logging
import os
import threading
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Callable

logger = logging.getLogger(__name__)

from .paths import default_cache_dir
CACHE_DIR = default_cache_dir() / "room"

# Model paths (HF cache)
FLUX_KLEIN_4B = "black-forest-labs/FLUX.2-klein-4B"
FLUX_SCHNELL_9B = "black-forest-labs/FLUX.1-schnell"
FLUX_KLEIN_9B = "black-forest-labs/FLUX.2-klein-9B"
SDXL_LIGHTNING = str(
    Path.home()
    / ".cache/huggingface/hub/models--ByteDance--SDXL-Lightning/snapshots"
    / "c9a24f48e1c025556787b0c58dd67a091ece2e44"
)

STYLE_SUFFIX = (
    "fantasy art, detailed environment painting, atmospheric lighting, "
    "medieval RPG, muted earthy tones, painterly style"
)

STYLE_SUFFIX_3D = (
    "photorealistic fantasy scene, cinematic depth of field, volumetric lighting, "
    "strong foreground midground background separation, atmospheric perspective, "
    "dramatic parallax depth layers, high detail textures, ray traced lighting, "
    "medieval dark fantasy, immersive 3D environment, wide angle perspective"
)

NEGATIVE_PROMPT = (
    "text, watermark, signature, blurry, low quality, modern, sci-fi, "
    "photograph, realistic photo, UI elements, cars, vehicles, asphalt, "
    "traffic lights, street lights, power lines, modern buildings, concrete, "
    "paved roads, contemporary, technology, electronics, modern clothing"
)

NEGATIVE_PROMPT_3D = (
    "text, watermark, signature, blurry, low quality, modern, sci-fi, "
    "UI elements, flat, 2D, cartoon, anime, illustration, clipart, "
    "no depth, uniform background, cars, vehicles, asphalt, traffic lights, "
    "street lights, power lines, modern buildings, concrete, paved roads, "
    "contemporary, technology, electronics, modern clothing"
)

OLLAMA_URL = "http://localhost:11434/api/generate"
OLLAMA_MODEL = "qwen2.5:3b"

LLM_SYSTEM_PROMPT = (
    "You convert MUD/text-game room descriptions into concise image generation prompts "
    "for a fantasy art AI. The game is set in a MEDIEVAL FANTASY world — all streets are "
    "cobblestone paths, all buildings are stone/timber/thatch, all lighting is torches/lanterns/"
    "candles. There are NO modern elements whatsoever. "
    "Extract ONLY visual elements: setting, lighting, objects, atmosphere, architecture. "
    "NEVER include people, figures, characters, NPCs, or any living beings in the prompt "
    "unless they are EXPLICITLY listed after 'Also here:' in the input. "
    "The scene should be EMPTY of people — show only the environment/room itself. "
    "Do NOT include game mechanics, exits, or directions. "
    "Do NOT include any thinking, explanation, or tags. "
    "Output ONLY the image prompt as a single paragraph, nothing else."
)

LLM_SYSTEM_PROMPT_3D = (
    "You convert MUD/text-game room descriptions into concise image generation prompts "
    "for a PHOTOREALISTIC 3D parallax scene set in a MEDIEVAL FANTASY world — all streets "
    "are cobblestone, all buildings are stone/timber/thatch, lighting is torches/lanterns. "
    "There are NO modern elements. The image will be rendered with depth-based "
    "camera movement, so emphasize DEPTH LAYERS and SPATIAL SEPARATION. "
    "Describe clear foreground objects, midground details "
    "(furniture, structures), and background (distant walls, sky, landscape). "
    "NEVER include people, figures, characters, NPCs, or any living beings in the prompt "
    "unless they are EXPLICITLY listed after 'Also here:' in the input. "
    "The scene should be EMPTY of people — show only the environment/room itself. "
    "Use cinematic language: depth of field, volumetric fog, atmospheric haze for distance. "
    "Emphasize texture detail and realistic lighting with shadows that convey depth. "
    "Do NOT include game mechanics, exits, or directions. "
    "Do NOT include any thinking, explanation, or tags. "
    "Output ONLY the image prompt as a single paragraph, nothing else."
)


@dataclass
class RenderRequest:
    room_name: str
    description: str
    monsters: list[str]
    items: list[str]
    exits: list[str]
    generation_id: int  # monotonic, for cancellation


class RoomRenderer:
    def __init__(
        self,
        model_type: str = "flux-klein-4b",
        width: int = 768,
        height: int = 512,
        steps: int = 4,
        on_image_ready: Optional[Callable[[str, str, bytes], None]] = None,
        on_thumbnail_ready: Optional[Callable[[str, bytes], None]] = None,
        get_entity_description: Optional[Callable[[str], Optional[str]]] = None,
    ):
        """
        Args:
            model_type: 'flux-klein-4b' or 'sdxl-lightning'
            width/height: generation resolution
            steps: inference steps (4 for both Lightning and Klein)
            on_image_ready: callback(room_name, prompt, png_bytes) called on completion
            on_thumbnail_ready: callback(entity_key, png_bytes) called when thumbnail done
        """
        self.model_type = model_type
        self.width = width
        self.height = height
        self.steps = steps
        self.on_image_ready = on_image_ready
        self.on_thumbnail_ready = on_thumbnail_ready
        self.get_entity_description = get_entity_description
        self.on_progress: Optional[Callable[[str, float], None]] = None
        self.on_thumb_progress: Optional[Callable[[str, float], None]] = None  # (entity_key, 0-1)
        self.depth_3d_enabled = False
        self.blocked = False  # when True, pipeline won't load (chatbot owns VRAM)

        self._pipe = None
        self._device = None
        self._dtype = None
        self._loaded = False
        self._loading = False
        self._lock = threading.Lock()
        self._gen_lock = threading.Lock()  # serialize pipeline usage (room + thumbnail)
        self._generation_id = 0
        self._current_gen_id = 0  # the one actively generating
        self._cache: dict[str, bytes] = {}  # hash -> png bytes
        self._gen_thread: Optional[threading.Thread] = None
        self._current_cache_key: Optional[str] = None  # track what we're generating
        self._thumb_queue: list[tuple[str, str, str]] = []  # (key, prompt, variant)
        self._thumb_thread: Optional[threading.Thread] = None
        self._room_queue: list[tuple[RenderRequest, str]] = []  # (req, cache_key)

        CACHE_DIR.mkdir(parents=True, exist_ok=True)

    def _cache_key(self, room_name: str, exits: list[str] = None) -> str:
        exits_str = '|'.join(sorted(exits)) if exits else ''
        content = f"{room_name}|{exits_str}"
        return hashlib.sha256(content.encode()).hexdigest()[:16]

    def _check_cache(self, key: str) -> Optional[bytes]:
        if key in self._cache:
            return self._cache[key]
        cache_file = CACHE_DIR / f"{key}.webp"
        if cache_file.exists():
            data = cache_file.read_bytes()
            self._cache[key] = data
            return data
        return None

    def invalidate_cache(self, room_name: str, exits: list[str] = None) -> None:
        """Delete cached image for a room so it regenerates."""
        key = self._cache_key(room_name, exits)
        self._cache.pop(key, None)
        cache_file = CACHE_DIR / f"{key}.webp"
        if cache_file.exists():
            cache_file.unlink()
            logger.info(f"Cache invalidated for: {room_name}")
        # Clear current_cache_key so request_render doesn't skip
        if self._current_cache_key == key:
            self._current_cache_key = None

    def _save_cache(self, key: str, data: bytes) -> None:
        self._cache[key] = data
        cache_file = CACHE_DIR / f"{key}.webp"
        cache_file.write_bytes(data)

    def _build_prompt(self, req: RenderRequest) -> str:
        # Build context for the LLM
        parts = [f"Room: {req.room_name}"]
        if req.description:
            parts.append(f"Description: {req.description}")
        if req.exits:
            parts.append(f"Visible passages/exits lead: {', '.join(req.exits)}")

        mud_text = "\n".join(parts)

        use_3d = self.depth_3d_enabled

        # Ask LLM to build the image prompt
        self._report(f"LLM prompt: {req.room_name}...")
        prompt = self._llm_build_prompt(mud_text, use_3d=use_3d)
        if not prompt:
            # Fallback if LLM fails
            prompt = self._fallback_prompt(req)

        # Append style suffix
        style = STYLE_SUFFIX_3D if use_3d else STYLE_SUFFIX
        prompt = f"{prompt}, {style}"
        logger.info(f"Prompt ({'3D' if use_3d else '2D'}): {prompt[:150]}...")
        return prompt

    def _llm_build_prompt(self, mud_text: str, use_3d: bool = False) -> Optional[str]:
        """Call local Ollama to convert MUD text into an image prompt."""
        try:
            system_prompt = LLM_SYSTEM_PROMPT_3D if use_3d else LLM_SYSTEM_PROMPT
            payload = json.dumps({
                "model": OLLAMA_MODEL,
                "system": system_prompt,
                "prompt": mud_text,
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

            # deepseek-r1 sometimes wraps in <think> tags — strip those
            if "<think>" in response:
                # Take everything after the last </think>
                parts = response.split("</think>")
                response = parts[-1].strip()

            # Clean up any remaining tags or quotes
            response = response.strip('"\'')

            if len(response) > 20:
                logger.info(f"LLM prompt: {response[:100]}...")
                return response
            else:
                logger.warning(f"LLM response too short: {response}")
                return None

        except Exception as e:
            logger.error(f"LLM prompt generation failed: {e}")
            return None

    def _fallback_prompt(self, req: RenderRequest) -> str:
        """Simple fallback if LLM is unavailable."""
        parts = []
        desc = req.description.strip()
        if desc:
            parts.append(desc[:200].rstrip(". "))
        if req.monsters:
            parts.append(", ".join(req.monsters))
        return ", ".join(parts) if parts else req.room_name

    def load_pipeline(self) -> None:
        """Load the model pipeline. Call from a background thread."""
        if self.blocked:
            logger.info("Pipeline load blocked (chatbot mode active)")
            return
        with self._lock:
            if self._loaded or self._loading:
                return
            self._loading = True
        logger.info(f"Loading {self.model_type} pipeline...")
        t0 = time.time()

        # Unload all Ollama models from VRAM first to free GPU memory
        try:
            import requests as _req
            ps = _req.get("http://localhost:11434/api/ps", timeout=5).json()
            for m in ps.get("models", []):
                model_name = m.get("name", "")
                _req.post(
                    "http://localhost:11434/api/generate",
                    json={"model": model_name, "keep_alive": 0},
                    timeout=10,
                )
                logger.info(f"Unloaded {model_name} from Ollama VRAM")
            if ps.get("models"):
                import time as _t
                _t.sleep(2)
        except Exception as e:
            logger.warning(f"Failed to unload Ollama models: {e}")

        import torch
        import gc
        gc.collect()
        if torch.cuda.is_available():
            torch.cuda.empty_cache()

        self._device = "cuda" if torch.cuda.is_available() else "cpu"

        if self.model_type == "flux-klein-9b":
            self._dtype = torch.bfloat16 if self._device == "cuda" else torch.float32
            try:
                from diffusers import FluxPipeline
                self._pipe = FluxPipeline.from_pretrained(
                    FLUX_KLEIN_9B, torch_dtype=self._dtype,
                    device_map=self._device,
                )
            except Exception:
                from diffusers import DiffusionPipeline
                self._pipe = DiffusionPipeline.from_pretrained(
                    FLUX_KLEIN_9B, torch_dtype=self._dtype,
                    device_map=self._device,
                )

        elif self.model_type == "flux-schnell-9b":
            self._dtype = torch.bfloat16 if self._device == "cuda" else torch.float32
            try:
                from diffusers import FluxPipeline
                self._pipe = FluxPipeline.from_pretrained(
                    FLUX_SCHNELL_9B, torch_dtype=self._dtype
                )
            except Exception:
                from diffusers import DiffusionPipeline
                self._pipe = DiffusionPipeline.from_pretrained(
                    FLUX_SCHNELL_9B, torch_dtype=self._dtype
                )
            self._pipe = self._pipe.to(self._device)

        elif self.model_type == "flux-klein-4b":
            self._dtype = torch.bfloat16 if self._device == "cuda" else torch.float32
            try:
                from diffusers import FluxPipeline
                self._pipe = FluxPipeline.from_pretrained(
                    FLUX_KLEIN_4B, torch_dtype=self._dtype
                )
            except Exception:
                from diffusers import DiffusionPipeline
                self._pipe = DiffusionPipeline.from_pretrained(
                    FLUX_KLEIN_4B, torch_dtype=self._dtype
                )
            self._pipe = self._pipe.to(self._device)

        # Enable flash attention 2 if available (for all flux models)
        if self.model_type.startswith("flux") and self._pipe is not None:
            try:
                self._pipe.transformer.enable_attn_implementation("flash_attention_2")
                logger.info("Flash Attention 2 enabled")
            except Exception:
                logger.info("Flash Attention 2 not available, using default attention")

        elif self.model_type == "sdxl-lightning":
            self._dtype = torch.float16 if self._device == "cuda" else torch.float32
            from diffusers import (
                StableDiffusionXLPipeline,
                UNet2DConditionModel,
                EulerDiscreteScheduler,
            )
            from safetensors.torch import load_file

            sdxl_base = None
            # Find SDXL base
            import glob
            cache_pattern = os.path.expanduser(
                "~/.cache/huggingface/hub/models--stabilityai--"
                "stable-diffusion-xl-base-1.0/snapshots/*/"
            )
            matches = glob.glob(cache_pattern)
            if matches:
                sdxl_base = matches[0].rstrip("/")

            if not sdxl_base:
                raise FileNotFoundError("SDXL base model not found in HF cache")

            unet_file = os.path.join(
                SDXL_LIGHTNING, "sdxl_lightning_4step_unet.safetensors"
            )
            unet = UNet2DConditionModel.from_config(sdxl_base, subfolder="unet")
            unet.load_state_dict(load_file(unet_file, device="cpu"))
            unet = unet.to(dtype=self._dtype)

            try:
                self._pipe = StableDiffusionXLPipeline.from_pretrained(
                    sdxl_base, unet=unet, torch_dtype=self._dtype, variant="fp16"
                )
            except (OSError, ValueError):
                self._pipe = StableDiffusionXLPipeline.from_pretrained(
                    sdxl_base, unet=unet, torch_dtype=self._dtype
                )
            self._pipe.scheduler = EulerDiscreteScheduler.from_config(
                self._pipe.scheduler.config, timestep_spacing="trailing"
            )
            self._pipe = self._pipe.to(self._device)
            if hasattr(self._pipe, "enable_attention_slicing"):
                self._pipe.enable_attention_slicing()

        elapsed = time.time() - t0
        self._loaded = True
        self._loading = False
        logger.info(f"Pipeline loaded in {elapsed:.1f}s")

    def offload_to_cpu(self) -> None:
        """Move pipeline to CPU to free GPU VRAM for depth processing."""
        if self._pipe is not None and self._device == "cuda":
            import torch
            logger.info("Offloading image pipeline to CPU to free VRAM...")
            self._pipe = self._pipe.to("cpu")
            torch.cuda.empty_cache()
            logger.info("Pipeline offloaded to CPU, VRAM freed")

    def reload_to_gpu(self) -> None:
        """Move pipeline back to GPU after depth processing."""
        if self._pipe is not None and self._device == "cuda":
            import torch
            logger.info("Reloading image pipeline to GPU...")
            self._pipe = self._pipe.to(self._device)
            logger.info("Pipeline reloaded to GPU")

    def request_render(self, room_name: str, description: str,
                       monsters: list[str], items: list[str],
                       exits: list[str]) -> None:
        """Request a room render. Queues if currently generating another room."""
        cache_key = self._cache_key(room_name, exits)

        # Skip if we're already generating this exact room+monsters+exits combo
        if cache_key == self._current_cache_key:
            logger.debug(f"Already generating {room_name}, skipping")
            return

        # Check cache first
        cached = self._check_cache(cache_key)
        if cached is not None:
            logger.info(f"Cache hit for {room_name}")
            if self.on_image_ready:
                self.on_image_ready(room_name, "", cached)
            return

        # Skip if already in queue
        if any(ck == cache_key for _, ck in self._room_queue):
            logger.debug(f"Already queued {room_name}, skipping")
            return

        req = RenderRequest(
            room_name=room_name,
            description=description,
            monsters=monsters,
            items=items,
            exits=exits,
            generation_id=0,  # set when actually generating
        )

        # If currently generating, queue it; otherwise start immediately
        if self._current_cache_key is not None:
            self._room_queue.append((req, cache_key))
            logger.info(f"Queued room render: {room_name} ({len(self._room_queue)} in queue)")
        else:
            self._start_render(req, cache_key)

    def _start_render(self, req: RenderRequest, cache_key: str) -> None:
        """Start generating a room image."""
        self._generation_id += 1
        req.generation_id = self._generation_id
        self._current_cache_key = cache_key

        self._gen_thread = threading.Thread(
            target=self._generate_worker, args=(req, cache_key), daemon=True
        )
        self._gen_thread.start()

    def _report(self, activity: str, progress: float = -1) -> None:
        """Report generation progress to the GUI."""
        if self.on_progress:
            self.on_progress(activity, progress)

    def _generate_worker(self, req: RenderRequest, cache_key: str) -> None:
        """Background thread: load pipeline if needed, then generate."""
        try:
            if not self._loaded:
                if self.blocked:
                    logger.info("Generation skipped (chatbot mode active)")
                    self._report("")
                    self._current_cache_key = None
                    return
                self._report("Loading image pipeline...")
                self.load_pipeline()

            if self._pipe is None:
                logger.info("Pipeline not available, skipping generation")
                self._report("")
                self._current_cache_key = None
                return

            # Check if cancelled
            if req.generation_id != self._generation_id:
                logger.info(f"Generation cancelled (moved to new room)")
                self._report("")
                return

            self._current_gen_id = req.generation_id
            self._report(f"Building prompt for {req.room_name}...")
            prompt = self._build_prompt(req)

            logger.info(f"Generating image for: {req.room_name}")
            self._report(f"Generating: {req.room_name} (step 0/{self.steps})")
            t0 = time.time()

            import torch
            import random

            total_steps = self.steps
            seed = random.randint(0, 2**32 - 1)
            generator = torch.Generator(device=self._device).manual_seed(seed)
            logger.info(f"Using seed {seed} for {req.room_name}")

            def _step_callback(pipe, step, timestep, kwargs):
                self._report(
                    f"Generating: {req.room_name} (step {step + 1}/{total_steps})",
                    (step + 1) / total_steps,
                )
                return kwargs

            gen_kwargs = dict(
                prompt=prompt,
                width=self.width,
                height=self.height,
                num_inference_steps=self.steps,
                guidance_scale=0.0 if self.model_type in ("sdxl-lightning", "flux-schnell-9b", "flux-klein-9b") else 1.0,
                callback_on_step_end=_step_callback,
                generator=generator,
            )

            # SDXL supports negative prompt, Flux does not
            if self.model_type == "sdxl-lightning":
                gen_kwargs["negative_prompt"] = (
                    NEGATIVE_PROMPT_3D if self.depth_3d_enabled else NEGATIVE_PROMPT
                )

            with torch.no_grad():
                image = self._pipe(**gen_kwargs).images[0]

            # Check if cancelled during generation
            if req.generation_id != self._generation_id:
                logger.info(f"Generation cancelled after completion")
                self._report("")
                return

            elapsed = time.time() - t0
            logger.info(f"Generated in {elapsed:.1f}s")
            self._report(f"Saving: {req.room_name} ({elapsed:.1f}s)", 1.0)

            # Convert to WebP bytes (lossy, quality 82 — good balance)
            import io
            buf = io.BytesIO()
            image.save(buf, format="WebP", quality=82)
            png_bytes = buf.getvalue()

            # Cache it
            self._save_cache(cache_key, png_bytes)

            # Deliver
            if self.on_image_ready:
                self.on_image_ready(req.room_name, prompt, png_bytes)

            self._report("")
            self._current_cache_key = None
            self._process_room_queue()

        except Exception as e:
            logger.error(f"Generation failed: {e}", exc_info=True)
            self._report(f"Generation failed: {e}")
            self._current_cache_key = None
            self._process_room_queue()

    def _process_room_queue(self) -> None:
        """Start the next queued room render if any."""
        while self._room_queue:
            req, cache_key = self._room_queue.pop(0)
            # Skip if already cached now
            if self._check_cache(cache_key) is not None:
                logger.info(f"Queue skip (cached): {req.room_name}")
                if self.on_image_ready:
                    self.on_image_ready(req.room_name, "", self._check_cache(cache_key))
                continue
            logger.info(f"Processing queued room: {req.room_name} ({len(self._room_queue)} remaining)")
            self._start_render(req, cache_key)
            return

    def generate_thumbnail(self, entity_key: str, prompt: str, variant: str) -> None:
        """Queue a 512x512 thumbnail for generation. Processed sequentially."""
        if self.blocked:
            return  # silently skip when chatbot mode is active
        self._thumb_queue.append((entity_key, prompt, variant))
        # Start the worker thread if not already running
        if self._thumb_thread is None or not self._thumb_thread.is_alive():
            self._thumb_thread = threading.Thread(
                target=self._thumbnail_worker, daemon=True
            )
            self._thumb_thread.start()

    def _thumbnail_worker(self) -> None:
        """Process thumbnail queue sequentially."""
        while self._thumb_queue:
            entity_key, prompt, variant = self._thumb_queue.pop(0)
            try:
                if not self._loaded:
                    if self._loading:
                        # Wait for pipeline to finish loading
                        while self._loading:
                            time.sleep(0.5)
                    else:
                        self.load_pipeline()

                if self._pipe is None:
                    logger.error(f"Pipeline not available for thumbnail: {entity_key}")
                    continue

                import torch
                import io

                logger.info(f"Generating thumbnail for: {entity_key}")
                remaining = len(self._thumb_queue)
                self._report(
                    f"Thumbnail: {entity_key}"
                    + (f" ({remaining} queued)" if remaining else "")
                )
                if self.on_thumb_progress:
                    self.on_thumb_progress(entity_key, 0.0)
                t0 = time.time()

                total_steps = self.steps

                def _thumb_step_cb(pipe, step, timestep, kwargs):
                    frac = (step + 1) / total_steps
                    self._report(
                        f"Thumbnail: {entity_key} (step {step + 1}/{total_steps})",
                        frac,
                    )
                    if self.on_thumb_progress:
                        self.on_thumb_progress(entity_key, frac)
                    return kwargs

                gen_kwargs = dict(
                    prompt=prompt,
                    width=512,
                    height=512,
                    num_inference_steps=self.steps,
                    guidance_scale=0.0 if self.model_type in ("sdxl-lightning", "flux-schnell-9b", "flux-klein-9b") else 1.0,
                    callback_on_step_end=_thumb_step_cb,
                )

                with torch.no_grad():
                    image = self._pipe(**gen_kwargs).images[0]

                buf = io.BytesIO()
                image.save(buf, format="WebP", quality=85)
                png_bytes = buf.getvalue()

                elapsed = time.time() - t0
                logger.info(f"Thumbnail for {entity_key} generated in {elapsed:.1f}s")

                if self.on_thumbnail_ready:
                    self.on_thumbnail_ready(entity_key, png_bytes)

            except Exception as e:
                logger.error(f"Thumbnail generation failed for {entity_key}: {e}")

        self._report("")

    def shutdown(self) -> None:
        """Release pipeline from VRAM."""
        self._generation_id += 1  # cancel any in-progress
        if self._pipe is not None:
            self._pipe.to("cpu")
            del self._pipe
            self._pipe = None
            import gc
            gc.collect()
            import torch
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
            logger.info("Pipeline released from VRAM")
        self._loaded = False
