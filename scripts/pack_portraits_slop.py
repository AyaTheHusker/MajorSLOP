"""Add character portraits into an existing .slop archive.

Appends portrait entries to the main slop file without loading all data into memory.
Keys: portrait_{race}_{class}_{gender} (e.g., portrait_human_warrior_male)

Run: ./python/bin/python3.12 scripts/pack_portraits_slop.py [--slop path/to/file.slop]
"""
import argparse
import struct
import shutil
import tempfile
from pathlib import Path

SLOP_MAGIC = b"SLOP"
ASSET_PLAYER_THUMB = 2


def read_index(path: Path) -> list[tuple[str, int, int, int]]:
    """Read just the index from a .slop file. Returns [(key, asset_type, offset, size), ...]"""
    entries = []
    with open(path, "rb") as f:
        magic = f.read(4)
        if magic != SLOP_MAGIC:
            raise ValueError(f"Not a SLOP file: {path}")
        version, count = struct.unpack("<II", f.read(8))
        for _ in range(count):
            key_len = struct.unpack("<H", f.read(2))[0]
            key = f.read(key_len).decode("utf-8")
            asset_type = struct.unpack("<B", f.read(1))[0]
            offset = struct.unpack("<Q", f.read(8))[0]
            size = struct.unpack("<I", f.read(4))[0]
            entries.append((key, asset_type, offset, size))
    return entries


def merge_portraits_into_slop(slop_path: Path, portrait_dir: Path):
    """Merge portrait images into an existing .slop file.

    Strategy: read existing index, build new index with portraits added,
    rewrite entire file streaming data from old file + new portrait files.
    """
    # Read existing index
    old_index = read_index(slop_path)
    print(f"Existing slop: {len(old_index)} entries in {slop_path.name}")

    # Remove any existing portrait_ entries (so we can re-run safely)
    old_index = [(k, t, o, s) for k, t, o, s in old_index if not k.startswith("portrait_")]
    print(f"After removing old portraits: {len(old_index)} entries")

    # Build portrait entries
    portrait_files = sorted(portrait_dir.glob("*.webp"))
    if not portrait_files:
        print("No portrait files found!")
        return

    portrait_data = {}
    for f in portrait_files:
        key = f"portrait_{f.stem}"
        portrait_data[key] = f.read_bytes()
    print(f"Portraits to add: {len(portrait_data)}")

    # Build combined index
    # All entries: old ones (with new offsets) + portrait entries
    all_entries = []  # (key, asset_type, data_bytes_or_None, old_offset, old_size)

    for key, asset_type, offset, size in old_index:
        all_entries.append((key, asset_type, None, offset, size))
    for key, data in portrait_data.items():
        all_entries.append((key, ASSET_PLAYER_THUMB, data, 0, len(data)))

    total_count = len(all_entries)

    # Calculate new index size
    header_size = 4 + 4 + 4  # SLOP + version + count
    index_size = 0
    for key, _, _, _, _ in all_entries:
        key_bytes = key.encode("utf-8")
        index_size += 2 + len(key_bytes) + 1 + 8 + 4

    data_start = header_size + index_size

    # Calculate new offsets
    new_offsets = []
    offset = data_start
    for key, asset_type, new_data, old_offset, size in all_entries:
        actual_size = len(new_data) if new_data is not None else size
        new_offsets.append((offset, actual_size))
        offset += actual_size

    # Write new file (stream to temp, then replace)
    tmp = tempfile.NamedTemporaryFile(
        dir=slop_path.parent, suffix=".slop.tmp", delete=False
    )
    tmp_path = Path(tmp.name)
    print(f"Writing to temp file: {tmp_path}")

    try:
        with open(tmp_path, "wb") as out:
            # Header
            out.write(SLOP_MAGIC)
            out.write(struct.pack("<II", 1, total_count))

            # Index
            for i, (key, asset_type, _, _, _) in enumerate(all_entries):
                key_bytes = key.encode("utf-8")
                new_offset, actual_size = new_offsets[i]
                out.write(struct.pack("<H", len(key_bytes)))
                out.write(key_bytes)
                out.write(struct.pack("<B", asset_type))
                out.write(struct.pack("<Q", new_offset))
                out.write(struct.pack("<I", actual_size))

            # Data — stream from old file for existing entries, write new data for portraits
            with open(slop_path, "rb") as old_f:
                for i, (key, asset_type, new_data, old_offset, size) in enumerate(all_entries):
                    if new_data is not None:
                        out.write(new_data)
                    else:
                        old_f.seek(old_offset)
                        # Stream in chunks to avoid loading huge entries into memory
                        remaining = size
                        while remaining > 0:
                            chunk = min(remaining, 8 * 1024 * 1024)  # 8MB chunks
                            out.write(old_f.read(chunk))
                            remaining -= chunk

                    if (i + 1) % 1000 == 0:
                        print(f"  Written {i+1}/{total_count} entries...")

        print(f"Replacing original file...")
        # Replace original
        tmp_path.replace(slop_path)
        portrait_mb = sum(len(d) for d in portrait_data.values()) / (1024 * 1024)
        print(f"Done! Added {len(portrait_data)} portraits ({portrait_mb:.1f} MB) to {slop_path.name}")
        print(f"Total entries: {total_count}")

    except Exception:
        tmp_path.unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser(description="Add portraits to .slop file")
    parser.add_argument("--slop", type=str, default=None, help="Path to .slop file")
    args = parser.parse_args()

    base = Path(__file__).parent.parent

    if args.slop:
        slop_path = Path(args.slop)
    else:
        slop_path = base / "dist" / "slop" / "Full1.11PBaked.slop"

    if not slop_path.exists():
        print(f"Slop file not found: {slop_path}")
        return

    # Prefer 2x upscaled, fall back to originals
    portrait_dir = base / "dist" / "portraits" / "2x"
    if not portrait_dir.exists() or not list(portrait_dir.glob("*.webp")):
        portrait_dir = base / "dist" / "portraits"

    merge_portraits_into_slop(slop_path, portrait_dir)


if __name__ == "__main__":
    main()
