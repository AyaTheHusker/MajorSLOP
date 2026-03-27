"""Upscale all portraits 2x using Real-ESRGAN via spandrel (same as dat_bake.py).

Run: ./python/bin/python3.12 scripts/upscale_portraits.py
"""
import gc
import glob
import io
import time
from pathlib import Path

import numpy as np
import torch
from PIL import Image


def main():
    import spandrel
    from torch.hub import download_url_to_file

    portrait_dir = Path(__file__).parent.parent / "dist" / "portraits"
    out_dir = portrait_dir / "2x"
    out_dir.mkdir(parents=True, exist_ok=True)

    # Use RealESRGAN_x2plus model
    model_cache = Path.home() / ".cache" / "mudproxy" / "models"
    model_cache.mkdir(parents=True, exist_ok=True)
    model_path = model_cache / "RealESRGAN_x2plus.pth"
    if not model_path.exists():
        print("Downloading RealESRGAN_x2plus.pth...")
        download_url_to_file(
            "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth",
            str(model_path),
        )

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    use_half = torch.cuda.is_available()
    model_desc = spandrel.ModelLoader().load_from_file(str(model_path))
    model = model_desc.eval().to(device)
    if use_half:
        model = model.half()
    print(f"Real-ESRGAN 2x loaded on {device} (scale={model_desc.scale})")

    TILE = 512
    TILE_PAD = 10

    files = sorted(glob.glob(str(portrait_dir / "*.webp")))
    print(f"Upscaling {len(files)} portraits to {out_dir}")

    t0 = time.time()
    done = 0
    skipped = 0
    errors = 0

    for i, fpath in enumerate(files):
        name = Path(fpath).stem
        outpath = out_dir / f"{name}.webp"
        if outpath.exists():
            skipped += 1
            continue

        try:
            img = Image.open(fpath).convert("RGB")
            w, h = img.size

            # Convert to tensor [1, 3, H, W] in 0-1 range
            img_np = np.array(img, dtype=np.float32) / 255.0
            img_tensor = torch.from_numpy(img_np).permute(2, 0, 1).unsqueeze(0).to(device)
            if use_half:
                img_tensor = img_tensor.half()

            # Tiled inference
            scale = model_desc.scale
            oh, ow = h * scale, w * scale
            out_tensor = torch.zeros((1, 3, oh, ow), dtype=img_tensor.dtype, device=device)

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

                    tile_in = img_tensor[:, :, py0:py1, px0:px1]
                    with torch.no_grad():
                        tile_out = model(tile_in)

                    ox0 = (x0 - px0) * scale
                    oy0 = (y0 - py0) * scale
                    ox1 = ox0 + (x1 - x0) * scale
                    oy1 = oy0 + (y1 - y0) * scale

                    out_tensor[:, :, y0*scale:y1*scale, x0*scale:x1*scale] = \
                        tile_out[:, :, oy0:oy1, ox0:ox1]

            out_np = (out_tensor.squeeze(0).permute(1, 2, 0).float().clamp(0, 1).cpu().numpy() * 255).astype(np.uint8)

            buf = io.BytesIO()
            Image.fromarray(out_np).save(buf, format="WebP", quality=92)
            outpath.write_bytes(buf.getvalue())
            done += 1
            size_kb = len(buf.getvalue()) / 1024
            print(f"[{i+1}/{len(files)}] {name} {w}x{h} -> {ow}x{oh} ({size_kb:.0f} KB)")

        except Exception as e:
            errors += 1
            print(f"[{i+1}/{len(files)}] {name} ERROR: {e}")

    del model, model_desc
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    elapsed = time.time() - t0
    print(f"\nDone: {done} upscaled, {skipped} skipped, {errors} errors in {elapsed:.0f}s")
    print(f"  Avg: {elapsed/max(done,1):.1f}s per image")
    print(f"  Output: {out_dir}")


if __name__ == "__main__":
    main()
