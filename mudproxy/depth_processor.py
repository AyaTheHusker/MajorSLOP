"""Depth map + LAMA inpainting processor for room images.

Generates depth maps using Depth Anything V2 and optional LAMA inpainting
for disocclusion fill. Results are cached per unique room image hash.
Uses the same scripts as the Scurry DepthCarousel system.
"""
import hashlib
import logging
import os
import subprocess
import threading
from pathlib import Path
from typing import Optional, Callable

logger = logging.getLogger(__name__)

from .paths import default_cache_dir
CACHE_DIR = default_cache_dir() / "depth"
SCURRY_SCRIPTS = Path.home() / "CodeBase" / "Scurry" / "scripts"
DEPTH_SCRIPT = SCURRY_SCRIPTS / "depth_estimate.py"
INPAINT_SCRIPT = SCURRY_SCRIPTS / "depth_inpaint.py"


class DepthProcessor:
    """Background depth map + inpainting generator with caching."""

    MAX_RETRIES = 2  # Max failures per room key before giving up

    def __init__(self, python_path: str = "python3",
                 on_depth_ready: Optional[Callable] = None,
                 inpaint_enabled: bool = True,
                 is_idle_cb: Optional[Callable[[], bool]] = None,
                 offload_gpu_cb: Optional[Callable] = None,
                 reload_gpu_cb: Optional[Callable] = None):
        """
        Args:
            python_path: Path to Python interpreter with torch/transformers installed.
            on_depth_ready: Callback(room_key, color_path, depth_path, inpaint_path)
                            called when depth processing completes for a room.
            inpaint_enabled: Whether to run LAMA inpainting after depth estimation.
            is_idle_cb: Optional callback that returns True when the image generator
                        is idle (no room renders or thumbnails pending). Depth
                        processing waits for this before starting.
        """
        self._python = python_path
        self._on_depth_ready = on_depth_ready
        self._inpaint_enabled = inpaint_enabled
        self._is_idle_cb = is_idle_cb
        self._offload_gpu_cb = offload_gpu_cb
        self._reload_gpu_cb = reload_gpu_cb
        self._queue: list[tuple[str, str]] = []  # (room_key, image_path)
        self._processing = False
        self._lock = threading.Lock()
        self._thread: Optional[threading.Thread] = None
        self._fail_counts: dict[str, int] = {}  # room_key -> failure count

        CACHE_DIR.mkdir(parents=True, exist_ok=True)

        # Check if scripts exist
        if not DEPTH_SCRIPT.exists():
            logger.warning(f"Depth estimation script not found: {DEPTH_SCRIPT}")
        if not INPAINT_SCRIPT.exists():
            logger.warning(f"Inpainting script not found: {INPAINT_SCRIPT}")

    def _cache_key(self, image_path: str) -> str:
        """Hash the image file content for cache key."""
        h = hashlib.sha256()
        with open(image_path, "rb") as f:
            for chunk in iter(lambda: f.read(8192), b""):
                h.update(chunk)
        return h.hexdigest()[:16]

    def get_cached_paths(self, room_key: str) -> tuple[Optional[str], Optional[str]]:
        """Return (depth_path, inpaint_path) if cached, else (None, None)."""
        depth_path = CACHE_DIR / f"{room_key}_depth.png"
        inpaint_path = CACHE_DIR / f"{room_key}_inpaint.png"
        d = str(depth_path) if depth_path.exists() else None
        i = str(inpaint_path) if inpaint_path.exists() else None
        return d, i

    def has_depth(self, room_key: str) -> bool:
        """Check if depth map exists in cache."""
        return (CACHE_DIR / f"{room_key}_depth.png").exists()

    def invalidate(self, room_key: str) -> None:
        """Delete cached depth map and inpaint for a room, and reset fail count."""
        for suffix in ("_depth.png", "_inpaint.png"):
            p = CACHE_DIR / f"{room_key}{suffix}"
            if p.exists():
                p.unlink()
                logger.info(f"Deleted depth cache: {p.name}")
        self._fail_counts.pop(room_key, None)

    def process_image(self, room_key: str, image_path: str) -> None:
        """Queue a room image for depth processing. Skips if already cached."""
        if self.has_depth(room_key):
            # Already cached — notify immediately
            depth_path, inpaint_path = self.get_cached_paths(room_key)
            if self._on_depth_ready:
                self._on_depth_ready(room_key, image_path, depth_path, inpaint_path)
            return

        # Skip if we've already failed too many times for this key
        if self._fail_counts.get(room_key, 0) >= self.MAX_RETRIES:
            return  # silently skip — already logged on failure

        with self._lock:
            # Don't queue duplicates
            if any(rk == room_key for rk, _ in self._queue):
                return
            self._queue.append((room_key, image_path))

        self._process_next()

    def _process_next(self) -> None:
        """Start processing next item in queue if not already running.

        Waits for the image generator to be idle before starting depth work
        so that 2D images and thumbnails always take priority.
        """
        with self._lock:
            if self._processing or not self._queue:
                return
            self._processing = True
            room_key, image_path = self._queue.pop(0)

        self._thread = threading.Thread(
            target=self._run_processing,
            args=(room_key, image_path),
            daemon=True,
        )
        self._thread.start()

    def _run_processing(self, room_key: str, image_path: str) -> None:
        """Run depth estimation + optional inpainting in background thread.

        Waits for the image generator to be idle first so 2D images and
        thumbnails always get priority over depth map generation.
        """
        import time as _time

        # Wait for image generator to be idle before consuming GPU
        if self._is_idle_cb:
            wait_count = 0
            while not self._is_idle_cb():
                _time.sleep(2)
                wait_count += 1
                if wait_count % 15 == 0:
                    logger.info(f"Depth waiting for renderer idle ({wait_count * 2}s)...")

        depth_path = str(CACHE_DIR / f"{room_key}_depth.png")
        inpaint_path = str(CACHE_DIR / f"{room_key}_inpaint.png")
        failed = False
        # Force CPU for subprocesses to avoid VRAM conflicts with FLUX/thumbnails
        env = {**os.environ, "CUDA_VISIBLE_DEVICES": ""}

        try:
            # Step 1: Depth estimation
            if not Path(depth_path).exists():
                logger.info(f"Generating depth map for {room_key}...")
                if not DEPTH_SCRIPT.exists():
                    logger.error("Depth estimation script not found")
                    failed = True
                    return
                result = subprocess.run(
                    [self._python, str(DEPTH_SCRIPT),
                     "--input", image_path,
                     "--output", depth_path,
                     "--model", "depth-anything/Depth-Anything-V2-Small-hf"],
                    capture_output=True, text=True, timeout=120,
                    env=env,
                )
                if result.returncode != 0 or not Path(depth_path).exists():
                    stderr = result.stderr
                    logger.error(f"Depth estimation failed for {room_key}: {stderr[:300]}")
                    failed = True
                    if "CUDA out of memory" in stderr or "OutOfMemoryError" in stderr:
                        logger.error("CUDA OOM during depth — stopping retries for this room")
                        self._fail_counts[room_key] = self.MAX_RETRIES  # immediate stop
                    return
                logger.info(f"Depth map ready: {room_key}")

            # Step 2: Inpainting (optional)
            actual_inpaint = None
            if self._inpaint_enabled and not Path(inpaint_path).exists():
                if INPAINT_SCRIPT.exists():
                    logger.info(f"Running LAMA inpainting for {room_key}...")
                    result = subprocess.run(
                        [self._python, str(INPAINT_SCRIPT),
                         "--image", image_path,
                         "--depth", depth_path,
                         "--output", inpaint_path],
                        capture_output=True, text=True, timeout=180,
                        env=env,
                    )
                    if result.returncode == 0 and Path(inpaint_path).exists():
                        actual_inpaint = inpaint_path
                        logger.info(f"Inpainting ready: {room_key}")
                    else:
                        logger.warning(f"Inpainting failed (non-fatal): {result.stderr[:200]}")
            elif Path(inpaint_path).exists():
                actual_inpaint = inpaint_path

            # Notify
            if self._on_depth_ready:
                self._on_depth_ready(room_key, image_path, depth_path, actual_inpaint)

        except subprocess.TimeoutExpired:
            logger.error(f"Depth processing timed out for {room_key}")
            failed = True
        except Exception as e:
            logger.error(f"Depth processing error for {room_key}: {e}", exc_info=True)
            failed = True
        finally:
            if failed:
                self._fail_counts[room_key] = self._fail_counts.get(room_key, 0) + 1
                count = self._fail_counts[room_key]
                logger.warning(f"Depth failure {count}/{self.MAX_RETRIES} for {room_key}")
            with self._lock:
                self._processing = False
            self._process_next()

    def batch_process_existing(self, room_image_dir: str) -> None:
        """Queue all existing room images in a directory for depth processing."""
        p = Path(room_image_dir)
        if not p.exists():
            return
        for img in p.glob("*.webp"):
            room_key = img.stem  # filename without extension
            if not self.has_depth(room_key):
                self._queue.append((room_key, str(img)))
        if self._queue:
            logger.info(f"Queued {len(self._queue)} room images for depth processing")
            self._process_next()
