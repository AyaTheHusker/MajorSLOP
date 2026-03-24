#!/usr/bin/env python3
"""Test the dat_bake pipeline: write slop → read slop → depth → inpaint → save.

Uses small synthetic images so it runs fast without needing real baked assets.
Run: ./python/bin/python3.12 test_bake_pipeline.py
"""
import io
import sys
import time
import tempfile
from pathlib import Path

# Make mudproxy importable
sys.path.insert(0, str(Path(__file__).parent))

from mudproxy.bake import write_slop, read_slop, ASSET_ROOM_IMAGE, ASSET_DEPTH_MAP, ASSET_INPAINT, ASSET_NPC_THUMB, ASSET_PROMPT, ASSET_METADATA

PASS = "\033[92m✓\033[0m"
FAIL = "\033[91m✗\033[0m"


def make_test_image(width=256, height=256, color=(100, 60, 40)):
    """Create a simple test image as WebP bytes."""
    from PIL import Image
    img = Image.new("RGB", (width, height), color)
    # Add some variation so depth estimation has something to work with
    from PIL import ImageDraw
    draw = ImageDraw.Draw(img)
    # Foreground rectangle (closer)
    draw.rectangle([60, 60, 180, 180], fill=(180, 140, 100))
    # Background gradient-ish
    for y in range(height):
        for x in range(0, 40):
            shade = int(40 + y * 0.3)
            img.putpixel((x, y), (shade, shade, shade + 20))
    buf = io.BytesIO()
    img.save(buf, format="WebP", quality=90)
    return buf.getvalue()


def test_write_read_slop():
    """Test 1: Write and read a .slop file with multiple asset types."""
    print("\n── Test 1: Write/Read .slop ──")

    img1 = make_test_image(256, 256, (100, 60, 40))
    img2 = make_test_image(256, 256, (40, 80, 120))
    img3 = make_test_image(256, 256, (60, 120, 60))
    npc_img = make_test_image(128, 128, (150, 50, 50))

    entries = {
        "room_test1": (ASSET_ROOM_IMAGE, img1),
        "room_test2": (ASSET_ROOM_IMAGE, img2),
        "room_test3": (ASSET_ROOM_IMAGE, img3),
        "goblin": (ASSET_NPC_THUMB, npc_img),
        "room_test1_prompt": (ASSET_PROMPT, b"A dark stone corridor with flickering torchlight"),
        "room_test2_prompt": (ASSET_PROMPT, b"A moonlit garden with cobblestone paths"),
    }

    with tempfile.NamedTemporaryFile(suffix=".slop", delete=False) as f:
        slop_path = Path(f.name)

    write_slop(slop_path, entries)
    size_kb = slop_path.stat().st_size / 1024
    print(f"  Written: {slop_path.name} ({size_kb:.1f} KB, {len(entries)} entries)")

    loaded = read_slop(slop_path)

    assert len(loaded) == len(entries), f"Expected {len(entries)} entries, got {len(loaded)}"
    assert "room_test1" in loaded
    assert "goblin" in loaded
    assert loaded["room_test1"][0] == ASSET_ROOM_IMAGE
    assert loaded["goblin"][0] == ASSET_NPC_THUMB
    assert loaded["room_test1"][1] == img1

    print(f"  {PASS} Write/read roundtrip OK — {len(loaded)} entries match")
    return slop_path


def test_depth_estimation(slop_path):
    """Test 2: Run depth estimation on room images from slop."""
    print("\n── Test 2: Depth Estimation (Depth Anything V2 Large) ──")

    import torch
    import numpy as np
    from PIL import Image

    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"  Device: {device}")

    entries = read_slop(slop_path)
    room_images = {k: data for k, (t, data) in entries.items() if t == ASSET_ROOM_IMAGE}
    print(f"  Room images to process: {len(room_images)}")

    from transformers import pipeline as hf_pipeline

    t0 = time.monotonic()
    depth_pipe = hf_pipeline(
        "depth-estimation",
        model="depth-anything/Depth-Anything-V2-Large-hf",
        device=device,
    )
    load_time = time.monotonic() - t0
    print(f"  Model loaded in {load_time:.1f}s")

    depth_results = {}
    for key, img_bytes in room_images.items():
        img = Image.open(io.BytesIO(img_bytes)).convert("RGB")
        t0 = time.monotonic()
        result = depth_pipe(img)
        elapsed = time.monotonic() - t0

        depth_pil = result["depth"]
        depth = np.array(depth_pil, dtype=np.float32)
        d_min, d_max = depth.min(), depth.max()
        depth_range = d_max - d_min

        if depth_range > 1e-8:
            depth_norm = (depth - d_min) / (d_max - d_min)
        else:
            depth_norm = np.zeros_like(depth)
        depth_8bit = (depth_norm * 255).astype(np.uint8)

        buf = io.BytesIO()
        Image.fromarray(depth_8bit, mode="L").save(buf, format="PNG")
        depth_bytes = buf.getvalue()
        depth_results[key] = depth_bytes

        print(f"  {PASS} {key}: {elapsed*1000:.0f}ms, range={depth_range:.1f}, "
              f"depth={len(depth_bytes)//1024}K")

    # Add depth entries to slop
    for key, depth_bytes in depth_results.items():
        entries[f"{key}_depth"] = (ASSET_DEPTH_MAP, depth_bytes)

    write_slop(slop_path, entries)
    print(f"  {PASS} Saved {len(depth_results)} depth maps back to slop")

    # Cleanup
    del depth_pipe
    import gc
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    return depth_results


def test_inpainting(slop_path):
    """Test 3: Run LaMa inpainting on rooms with depth maps."""
    print("\n── Test 3: LaMa Inpainting ──")

    import torch
    import numpy as np
    import cv2
    from PIL import Image
    from simple_lama_inpainting import SimpleLama

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"  Device: {device}")

    entries = read_slop(slop_path)

    # Find rooms that have both image and depth
    room_keys = [k for k, (t, _) in entries.items() if t == ASSET_ROOM_IMAGE]
    rooms_with_depth = [k for k in room_keys if f"{k}_depth" in entries]
    print(f"  Rooms with depth: {len(rooms_with_depth)}/{len(room_keys)}")

    t0 = time.monotonic()
    lama = SimpleLama(device=device)
    load_time = time.monotonic() - t0
    print(f"  LaMa loaded in {load_time:.1f}s")

    DILATION = 20
    THRESHOLD = 25
    inpaint_results = {}

    for key in rooms_with_depth:
        img_bytes = entries[key][1]
        depth_bytes = entries[f"{key}_depth"][1]

        image = Image.open(io.BytesIO(img_bytes)).convert("RGB")
        depth = Image.open(io.BytesIO(depth_bytes)).convert("L")

        if depth.size != image.size:
            depth = depth.resize(image.size, Image.BILINEAR)

        depth_np = np.array(depth, dtype=np.float32)
        depth_u8 = np.array(depth, dtype=np.uint8)

        t0 = time.monotonic()

        # Sobel edge detection
        sobel_x = cv2.Sobel(depth_u8, cv2.CV_64F, 1, 0, ksize=3)
        sobel_y = cv2.Sobel(depth_u8, cv2.CV_64F, 0, 1, ksize=3)
        gradient_mag = np.sqrt(sobel_x ** 2 + sobel_y ** 2)

        grad_max = gradient_mag.max()
        if grad_max < 1e-8:
            print(f"  {key}: no edges, skipped")
            continue

        gradient_norm = (gradient_mag / grad_max * 255.0).astype(np.uint8)
        _, edge_mask = cv2.threshold(gradient_norm, THRESHOLD, 255, cv2.THRESH_BINARY)

        # Dilate toward background
        kernel_size = DILATION * 2 + 1
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (kernel_size, kernel_size))
        dilated_edges = cv2.dilate(edge_mask, kernel, iterations=1)

        depth_blurred = cv2.GaussianBlur(depth_np, (0, 0), sigmaX=DILATION)
        background_side = depth_np < depth_blurred

        disocclusion_mask = np.zeros_like(edge_mask)
        disocclusion_mask[(dilated_edges > 0) & background_side] = 255

        small_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
        thin_edge = cv2.dilate(edge_mask, small_kernel, iterations=1)
        disocclusion_mask[thin_edge > 0] = 255

        mask_pixels = (disocclusion_mask > 0).sum()
        total_pixels = disocclusion_mask.shape[0] * disocclusion_mask.shape[1]
        mask_pct = mask_pixels / total_pixels * 100

        if disocclusion_mask.max() == 0:
            print(f"  {key}: empty mask, skipped")
            continue

        mask_pil = Image.fromarray(disocclusion_mask, mode="L")
        result = lama(image, mask_pil)

        elapsed = time.monotonic() - t0

        buf = io.BytesIO()
        result.save(buf, format="WebP", quality=90)
        inpaint_bytes = buf.getvalue()
        inpaint_results[key] = inpaint_bytes

        print(f"  {PASS} {key}: {elapsed*1000:.0f}ms, mask={mask_pct:.1f}%, "
              f"inpaint={len(inpaint_bytes)//1024}K")

    # Save inpaint entries back
    for key, inpaint_bytes in inpaint_results.items():
        entries[f"{key}_inpaint"] = (ASSET_INPAINT, inpaint_bytes)

    write_slop(slop_path, entries)
    print(f"  {PASS} Saved {len(inpaint_results)} inpaint textures back to slop")

    # Cleanup
    del lama
    import gc
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    return inpaint_results


def test_final_load(slop_path):
    """Test 4: Verify the final slop has all asset types."""
    print("\n── Test 4: Final Verification ──")

    entries = read_slop(slop_path)

    counts = {}
    type_names = {
        ASSET_ROOM_IMAGE: "Room", ASSET_DEPTH_MAP: "Depth", ASSET_INPAINT: "Inpaint",
        ASSET_NPC_THUMB: "NPC", ASSET_PROMPT: "Prompt", ASSET_METADATA: "Meta",
    }
    for key, (t, data) in entries.items():
        name = type_names.get(t, f"type_{t}")
        counts[name] = counts.get(name, 0) + 1

    size_kb = slop_path.stat().st_size / 1024
    print(f"  File: {slop_path.name} ({size_kb:.1f} KB)")
    print(f"  Total entries: {len(entries)}")
    for name, count in sorted(counts.items()):
        print(f"    {name}: {count}")

    # Verify rooms have all three: image + depth + inpaint
    room_keys = [k for k, (t, _) in entries.items() if t == ASSET_ROOM_IMAGE]
    for key in room_keys:
        has_depth = f"{key}_depth" in entries
        has_inpaint = f"{key}_inpaint" in entries
        status = f"depth={'yes' if has_depth else 'NO'} inpaint={'yes' if has_inpaint else 'NO'}"
        marker = PASS if (has_depth and has_inpaint) else FAIL
        print(f"  {marker} {key}: {status}")

    all_complete = all(
        f"{k}_depth" in entries and f"{k}_inpaint" in entries
        for k in room_keys
    )

    if all_complete:
        print(f"\n  {PASS} ALL TESTS PASSED — full pipeline working")
    else:
        print(f"\n  {FAIL} Some rooms missing depth or inpaint")

    return all_complete


def main():
    print("=" * 60)
    print("  dat_bake pipeline test")
    print("  (synthetic images — no real bake needed)")
    print("=" * 60)

    slop_path = test_write_read_slop()

    try:
        test_depth_estimation(slop_path)
        test_inpainting(slop_path)
        test_final_load(slop_path)
    finally:
        # Cleanup temp file
        slop_path.unlink(missing_ok=True)
        print(f"\n  Cleaned up {slop_path.name}")


if __name__ == "__main__":
    main()
