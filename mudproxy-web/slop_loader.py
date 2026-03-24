"""Load .slop archives and serve assets by key."""
import hashlib
import logging
import struct
from pathlib import Path

logger = logging.getLogger(__name__)

SLOP_MAGIC = b"SLOP"

ASSET_NPC_THUMB = 0
ASSET_ITEM_THUMB = 1
ASSET_PLAYER_THUMB = 2
ASSET_ROOM_IMAGE = 3
ASSET_DEPTH_MAP = 4
ASSET_PROMPT = 5
ASSET_METADATA = 6

CREATURE_PREFIXES = {"large", "small", "big", "short", "fat", "fierce", "nasty", "thin", "angry", "happy", "tall"}

ASSET_TYPE_NAMES = {
    ASSET_NPC_THUMB: "npc_thumb",
    ASSET_ITEM_THUMB: "item_thumb",
    ASSET_PLAYER_THUMB: "player_thumb",
    ASSET_ROOM_IMAGE: "room_image",
    ASSET_DEPTH_MAP: "depth_map",
    ASSET_PROMPT: "prompt",
    ASSET_METADATA: "metadata",
}


class SlopEntry:
    __slots__ = ("key", "asset_type", "offset", "size", "slop_path")

    def __init__(self, key: str, asset_type: int, offset: int, size: int, slop_path: Path):
        self.key = key
        self.asset_type = asset_type
        self.offset = offset
        self.size = size
        self.slop_path = slop_path


class SlopLoader:
    def __init__(self, slop_dir: Path | None = None, cache_dir: Path | None = None):
        self._slop_dir = slop_dir or (Path.home() / ".cache" / "mudproxy" / "slop")
        self._cache_dir = cache_dir or (Path.home() / ".cache" / "mudproxy")
        # key -> SlopEntry (indexed for fast seek-based reads)
        self._index: dict[str, SlopEntry] = {}
        # Separate prompt index
        self._prompt_index: dict[str, SlopEntry] = {}
        self._metadata: dict = {}
        self._loaded_files: list[Path] = []

    def load_all(self) -> int:
        """Load indexes from all .slop files in slop_dir. Returns total entry count."""
        if not self._slop_dir.exists():
            return 0
        for slop_path in sorted(self._slop_dir.glob("*.slop")):
            self._load_slop_index(slop_path)
        logger.info(f"SlopLoader: {len(self._index)} assets, {len(self._prompt_index)} prompts "
                     f"from {len(self._loaded_files)} files")
        return len(self._index)

    def reload_all(self) -> int:
        """Re-scan slop dir, loading any new files. Returns total entry count."""
        if not self._slop_dir.exists():
            return 0
        for slop_path in sorted(self._slop_dir.glob("*.slop")):
            if slop_path not in self._loaded_files:
                self._load_slop_index(slop_path)
        return len(self._index)

    def load_file(self, path: Path) -> int:
        """Load a specific .slop file. Returns entry count."""
        return self._load_slop_index(path)

    def _load_slop_index(self, path: Path) -> int:
        """Read just the index of a .slop file (no data loaded into memory)."""
        count = 0
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic != SLOP_MAGIC:
                logger.warning(f"Not a SLOP file: {path}")
                return 0
            version, entry_count = struct.unpack("<II", f.read(8))

            for _ in range(entry_count):
                key_len = struct.unpack("<H", f.read(2))[0]
                key = f.read(key_len).decode("utf-8")
                asset_type = struct.unpack("<B", f.read(1))[0]
                offset = struct.unpack("<Q", f.read(8))[0]
                size = struct.unpack("<I", f.read(4))[0]

                entry = SlopEntry(key, asset_type, offset, size, path)

                if asset_type == ASSET_METADATA:
                    # Read metadata immediately (small)
                    pos = f.tell()
                    f.seek(offset)
                    import json
                    try:
                        self._metadata = json.loads(f.read(size).decode("utf-8"))
                    except Exception:
                        pass
                    f.seek(pos)
                elif asset_type == ASSET_PROMPT:
                    self._prompt_index[key] = entry
                else:
                    self._index[key] = entry
                count += 1

        self._loaded_files.append(path)
        logger.info(f"Indexed {count} entries from {path.name}")
        return count

    def get_asset(self, key: str) -> bytes | None:
        """Read asset data by key (seek-based, not cached in memory)."""
        entry = self._index.get(key)
        if not entry:
            return None
        with open(entry.slop_path, "rb") as f:
            f.seek(entry.offset)
            return f.read(entry.size)

    def get_prompt(self, key: str) -> str | None:
        """Get the prompt text for an asset key."""
        prompt_key = f"{key}_prompt"
        entry = self._prompt_index.get(prompt_key)
        if not entry:
            return None
        with open(entry.slop_path, "rb") as f:
            f.seek(entry.offset)
            return f.read(entry.size).decode("utf-8", errors="replace")

    def get_asset_type(self, key: str) -> int | None:
        entry = self._index.get(key)
        return entry.asset_type if entry else None

    def has_asset(self, key: str) -> bool:
        return key in self._index

    def get_room_image_key(self, room_name: str, exits: list[str]) -> str | None:
        """Look up room image key by name+exits hash (matches proxy cache key)."""
        exits_str = '|'.join(sorted(exits))
        cache_key = "room_" + hashlib.sha256(
            f"{room_name}|{exits_str}".encode()
        ).hexdigest()[:16]
        return cache_key if cache_key in self._index else None

    def get_entity_thumb_key(self, name: str) -> str | None:
        """Look up entity thumbnail by name, stripping one creature prefix if needed."""
        key = name.strip().lower()
        if key in self._index:
            return key
        # Strip ONE known prefix only (big slaver → slaver, NOT fat angry slaver → slaver)
        words = key.split()
        if len(words) > 1 and words[0] in CREATURE_PREFIXES:
            base = " ".join(words[1:])
            if base in self._index:
                return base
        return None

    def get_depth_key(self, room_key: str) -> str | None:
        """Get depth map key for a room image key."""
        depth_key = f"{room_key}_depth"
        return depth_key if depth_key in self._index else None

    def list_assets(self, asset_type: int | None = None) -> list[str]:
        """List all asset keys, optionally filtered by type."""
        if asset_type is None:
            return list(self._index.keys())
        return [k for k, e in self._index.items() if e.asset_type == asset_type]

    def get_stats(self) -> dict:
        """Get summary stats."""
        counts = {}
        for entry in self._index.values():
            tname = ASSET_TYPE_NAMES.get(entry.asset_type, "unknown")
            counts[tname] = counts.get(tname, 0) + 1
        return {
            "files": len(self._loaded_files),
            "total_assets": len(self._index),
            "total_prompts": len(self._prompt_index),
            "counts": counts,
            "metadata": self._metadata,
        }

    # ── Loose file fallback ──

    def get_loose_room(self, room_name: str, exits: list[str]) -> bytes | None:
        """Check loose room cache files."""
        exits_str = '|'.join(sorted(exits))
        cache_key = hashlib.sha256(
            f"{room_name}|{exits_str}".encode()
        ).hexdigest()[:16]
        path = self._cache_dir / "room_images" / f"{cache_key}.webp"
        if path.exists():
            return path.read_bytes()
        return None

    def get_loose_depth(self, room_name: str, exits: list[str]) -> bytes | None:
        """Check loose depth cache files."""
        exits_str = '|'.join(sorted(exits))
        cache_key = hashlib.sha256(
            f"{room_name}|{exits_str}".encode()
        ).hexdigest()[:16]
        path = self._cache_dir / "depth" / f"{cache_key}.png"
        if path.exists():
            return path.read_bytes()
        return None

    def get_loose_entity(self, name: str) -> bytes | None:
        """Check loose entity thumbnail cache."""
        safe = hashlib.sha256(name.strip().lower().encode()).hexdigest()[:16]
        for subdir in ("entities",):
            path = self._cache_dir / subdir / f"{safe}.webp"
            if path.exists():
                return path.read_bytes()
        return None
