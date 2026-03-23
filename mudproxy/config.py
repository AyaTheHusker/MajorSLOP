import json
from dataclasses import dataclass, field, asdict
from pathlib import Path

CONFIG_DIR = Path.home() / ".config" / "mudproxy"
CONFIG_FILE = CONFIG_DIR / "settings.json"


@dataclass
class AutoLoginTrigger:
    pattern: str
    response: str
    delay_ms: int = 500


@dataclass
class Config:
    bbs_host: str = "sos-bbs.net"
    bbs_port: int = 23
    proxy_host: str = "127.0.0.1"
    proxy_port: int = 9999
    auto_reconnect: bool = True
    reconnect_delay: int = 5
    username: str = ""
    password: str = ""
    character_name: str = ""  # MajorMUD character name (for self-look, inventory)
    auto_login_triggers: list[dict] = field(default_factory=list)

    # Depth 3D settings
    depth_3d_enabled: bool = False
    depth_3d_params: dict = field(default_factory=lambda: {
        "depth_scale": 0.15,
        "camera_mode": 3,
        "camera_intensity": 0.3,
        "camera_speed": 0.4,
        "isometric": 0.0,
        "steady": 0.0,
        "overscan": 0.05,
        "pan_speed": 0.1,
        "pan_amount_x": 0.3,
        "pan_amount_y": 0.2,
        "pan_phase": 0.0,
        "pan_shape": 0.0,
        "edge_fade_start": 0.15,
        "edge_fade_end": 0.4,
        "inpaint_blend_start": 0.02,
        "inpaint_blend_end": 0.08,
        "depth_contrast": 1.0,
        "bg_r": 0.1,
        "bg_g": 0.1,
        "bg_b": 0.18,
    })
    depth_inpaint_enabled: bool = True
    portrait_style: str = ""

    def save(self) -> None:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        data = asdict(self)
        CONFIG_FILE.write_text(json.dumps(data, indent=2))

    @classmethod
    def load(cls) -> "Config":
        if CONFIG_FILE.exists():
            try:
                data = json.loads(CONFIG_FILE.read_text())
                return cls(**{k: v for k, v in data.items()
                             if k in cls.__dataclass_fields__})
            except (json.JSONDecodeError, TypeError):
                pass
        return cls()
