"""Vulkan depth-parallax renderer — Python ctypes wrapper around libdepthrender.so.

Renders DepthFlow-style 2.5D parallax from a color image + depth map using
a Vulkan compute shader compiled to SPIR-V.
"""
import ctypes
import logging
import os
import subprocess
import threading
import time as _time
from pathlib import Path
from typing import Optional, Callable

logger = logging.getLogger(__name__)

_HERE = Path(__file__).parent
_NATIVE_DIR = _HERE / "native"
_SHADER_DIR = _HERE / "shaders"
_LIB_PATH = _NATIVE_DIR / "libdepthrender.so"
_SHADER_SRC = _SHADER_DIR / "depth_parallax.comp"
_SHADER_SPV = _SHADER_DIR / "depth_parallax.comp.spv"


def _ensure_built() -> bool:
    """Compile the native library + shader if missing."""
    ok = True

    # Compile SPIR-V if needed
    if not _SHADER_SPV.exists() or _SHADER_SRC.stat().st_mtime > _SHADER_SPV.stat().st_mtime:
        logger.info("Compiling depth parallax shader to SPIR-V...")
        try:
            subprocess.run(
                ["glslc", "-fshader-stage=compute", str(_SHADER_SRC), "-o", str(_SHADER_SPV)],
                check=True, capture_output=True, text=True,
            )
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            logger.error(f"Shader compilation failed: {e}")
            ok = False

    # Compile C library if needed
    c_src = _NATIVE_DIR / "depth_render.c"
    if not _LIB_PATH.exists() or c_src.stat().st_mtime > _LIB_PATH.stat().st_mtime:
        logger.info("Compiling Vulkan depth renderer...")
        try:
            subprocess.run(
                ["gcc", "-shared", "-fPIC", "-O2", "-o", str(_LIB_PATH),
                 str(c_src), "-lvulkan", "-lm"],
                check=True, capture_output=True, text=True,
            )
        except (subprocess.CalledProcessError, FileNotFoundError) as e:
            logger.error(f"Native library compilation failed: {e}")
            ok = False

    return ok


# Camera mode names for UI
CAMERA_MODES = [
    "Carousel",   # 0
    "Horizontal",  # 1
    "Vertical",    # 2
    "Circle",      # 3
    "Zoom",        # 4
    "Dolly",       # 5
    "Orbital",     # 6
    "Explore",     # 7
]

# Good default settings per camera mode
# Keys: depth_scale, camera_intensity, camera_speed, isometric, steady,
#        overscan, pan_speed, pan_amount_x, pan_amount_y
CAMERA_MODE_DEFAULTS = {
    0: {"depth_scale": 0.12, "camera_intensity": 0.15, "camera_speed": 0.25,
        "isometric": 0.0, "steady": 0.0, "overscan": 0.05,
        "pan_speed": 0.08, "pan_amount_x": 0.3, "pan_amount_y": 0.2},
    1: {"depth_scale": 0.15, "camera_intensity": 0.25, "camera_speed": 0.30,
        "isometric": 0.0, "steady": 0.0, "overscan": 0.05,
        "pan_speed": 0.06, "pan_amount_x": 0.2, "pan_amount_y": 0.0},
    2: {"depth_scale": 0.15, "camera_intensity": 0.20, "camera_speed": 0.25,
        "isometric": 0.0, "steady": 0.0, "overscan": 0.05,
        "pan_speed": 0.06, "pan_amount_x": 0.0, "pan_amount_y": 0.2},
    3: {"depth_scale": 0.15, "camera_intensity": 0.20, "camera_speed": 0.30,
        "isometric": 0.0, "steady": 0.0, "overscan": 0.05,
        "pan_speed": 0.08, "pan_amount_x": 0.2, "pan_amount_y": 0.15},
    4: {"depth_scale": 0.20, "camera_intensity": 0.30, "camera_speed": 0.20,
        "isometric": 0.0, "steady": 0.3, "overscan": 0.06,
        "pan_speed": 0.05, "pan_amount_x": 0.15, "pan_amount_y": 0.1},
    5: {"depth_scale": 0.18, "camera_intensity": 0.30, "camera_speed": 0.20,
        "isometric": 0.3, "steady": 0.2, "overscan": 0.06,
        "pan_speed": 0.05, "pan_amount_x": 0.2, "pan_amount_y": 0.15},
    6: {"depth_scale": 0.18, "camera_intensity": 0.25, "camera_speed": 0.25,
        "isometric": 0.2, "steady": 0.0, "overscan": 0.05,
        "pan_speed": 0.07, "pan_amount_x": 0.25, "pan_amount_y": 0.15},
    7: {"depth_scale": 0.15, "camera_intensity": 0.20, "camera_speed": 0.15,
        "isometric": 0.1, "steady": 0.15, "overscan": 0.06,
        "pan_speed": 0.04, "pan_amount_x": 0.25, "pan_amount_y": 0.2},
}


class DepthRenderParams:
    """Camera and rendering parameters for the parallax shader."""

    def __init__(self):
        self.depth_scale: float = 0.15
        self.camera_mode: int = 3        # Circle
        self.camera_intensity: float = 0.3
        self.camera_speed: float = 0.4
        self.isometric: float = 0.0
        self.steady: float = 0.0
        self.overscan: float = 0.05
        self.pan_speed: float = 0.1
        self.pan_amount_x: float = 0.3
        self.pan_amount_y: float = 0.2
        self.pan_phase: float = 0.0
        self.pan_shape: float = 0.0      # 0=circle, 1=figure-8, 2=lissajous
        self.edge_fade_start: float = 0.15
        self.edge_fade_end: float = 0.4
        self.inpaint_blend_start: float = 0.02
        self.inpaint_blend_end: float = 0.08
        self.depth_contrast: float = 1.0
        self.bg_r: float = 0.1
        self.bg_g: float = 0.1
        self.bg_b: float = 0.18

    def to_push_constants(self, t: float, width: int, height: int) -> list[float]:
        """Pack into push constant array matching shader layout."""
        return [
            t,
            float(width), float(height),
            self.depth_scale,
            float(self.camera_mode),
            self.camera_intensity,
            self.camera_speed,
            self.isometric,
            self.steady,
            self.overscan,
            self.pan_speed,
            self.pan_amount_x, self.pan_amount_y,
            self.pan_phase, self.pan_shape,
            self.edge_fade_start, self.edge_fade_end,
            self.inpaint_blend_start, self.inpaint_blend_end,
            self.depth_contrast,
            self.bg_r, self.bg_g, self.bg_b,
        ]

    def to_dict(self) -> dict:
        """Serialize to dict for config persistence."""
        return {
            "depth_scale": self.depth_scale,
            "camera_mode": self.camera_mode,
            "camera_intensity": self.camera_intensity,
            "camera_speed": self.camera_speed,
            "isometric": self.isometric,
            "steady": self.steady,
            "overscan": self.overscan,
            "pan_speed": self.pan_speed,
            "pan_amount_x": self.pan_amount_x,
            "pan_amount_y": self.pan_amount_y,
            "pan_phase": self.pan_phase,
            "pan_shape": self.pan_shape,
            "edge_fade_start": self.edge_fade_start,
            "edge_fade_end": self.edge_fade_end,
            "inpaint_blend_start": self.inpaint_blend_start,
            "inpaint_blend_end": self.inpaint_blend_end,
            "depth_contrast": self.depth_contrast,
            "bg_r": self.bg_r,
            "bg_g": self.bg_g,
            "bg_b": self.bg_b,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "DepthRenderParams":
        p = cls()
        for k, v in d.items():
            if hasattr(p, k):
                setattr(p, k, v)
        return p


class VulkanDepthRenderer:
    """Offscreen Vulkan compute renderer for depth parallax effect."""

    def __init__(self, width: int = 768, height: int = 512):
        self._width = width
        self._height = height
        self._lib = None
        self._handle = None
        self._available = False

        if not _ensure_built():
            logger.warning("Vulkan depth renderer not available (build failed)")
            return

        try:
            self._lib = ctypes.CDLL(str(_LIB_PATH))
        except OSError as e:
            logger.warning(f"Cannot load libdepthrender.so: {e}")
            return

        # Set function signatures
        self._lib.dr_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
        self._lib.dr_create.restype = ctypes.c_void_p

        self._lib.dr_load_scene.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int,  # color
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int,  # depth
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_int, ctypes.c_int,  # inpaint
        ]
        self._lib.dr_load_scene.restype = ctypes.c_int

        self._lib.dr_render_frame.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_float), ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint8),
        ]
        self._lib.dr_render_frame.restype = ctypes.c_int

        self._lib.dr_destroy.argtypes = [ctypes.c_void_p]
        self._lib.dr_destroy.restype = None

        # Create renderer
        self._handle = self._lib.dr_create(width, height, str(_SHADER_SPV).encode())
        if not self._handle:
            logger.warning("Vulkan depth renderer creation failed")
            return

        self._available = True
        # Pre-allocate output buffer
        self._output_buf = (ctypes.c_uint8 * (width * height * 4))()
        logger.info(f"Vulkan depth renderer ready ({width}x{height})")

    @property
    def available(self) -> bool:
        return self._available

    def load_scene(self, color_rgba: bytes, color_w: int, color_h: int,
                   depth_gray: bytes, depth_w: int, depth_h: int,
                   inpaint_rgba: Optional[bytes] = None,
                   inpaint_w: int = 0, inpaint_h: int = 0) -> bool:
        """Upload color, depth, and optional inpaint textures to GPU."""
        if not self._available:
            return False

        c_buf = (ctypes.c_uint8 * len(color_rgba)).from_buffer_copy(color_rgba)
        d_buf = (ctypes.c_uint8 * len(depth_gray)).from_buffer_copy(depth_gray)

        if inpaint_rgba:
            i_buf = (ctypes.c_uint8 * len(inpaint_rgba)).from_buffer_copy(inpaint_rgba)
            i_ptr = ctypes.cast(i_buf, ctypes.POINTER(ctypes.c_uint8))
        else:
            i_ptr = ctypes.cast(ctypes.c_void_p(0), ctypes.POINTER(ctypes.c_uint8))
            inpaint_w = 0
            inpaint_h = 0

        result = self._lib.dr_load_scene(
            self._handle,
            ctypes.cast(c_buf, ctypes.POINTER(ctypes.c_uint8)), color_w, color_h,
            ctypes.cast(d_buf, ctypes.POINTER(ctypes.c_uint8)), depth_w, depth_h,
            i_ptr, inpaint_w, inpaint_h,
        )
        return result == 0

    def render_frame(self, t: float, params: DepthRenderParams) -> Optional[bytes]:
        """Render one frame. Returns RGBA bytes or None on failure."""
        if not self._available:
            return None

        pc = params.to_push_constants(t, self._width, self._height)
        pc_arr = (ctypes.c_float * len(pc))(*pc)

        result = self._lib.dr_render_frame(
            self._handle, pc_arr, len(pc), self._output_buf
        )
        if result != 0:
            return None
        return bytes(self._output_buf)

    def shutdown(self):
        if self._handle and self._lib:
            self._lib.dr_destroy(self._handle)
            self._handle = None
            self._available = False
