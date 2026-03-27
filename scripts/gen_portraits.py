"""Batch-generate character portraits for Race × Class × Gender combos.

Uses the same FLUX pipeline as bake.py. Outputs WebP portraits to dist/portraits/.

Run as: ./python/bin/python3.12 scripts/gen_portraits.py [--model flux-dev] [--limit 5]
"""
import argparse
import gc
import io
import json
import sys
import time
from pathlib import Path

# Reuse prompt builder and data from gen_portrait_prompts
sys.path.insert(0, str(Path(__file__).parent))
from gen_portrait_prompts import RACES, CLASSES, GENDERS, build_prompt

# Model configs (same as bake.py)
MODELS = {
    "flux-dev": {
        "name": "FLUX.2 Dev (4-bit)",
        "repo": "diffusers/FLUX.2-dev-bnb-4bit",
        "guidance": 4.0,
        "steps": 28,
    },
    "flux-klein-9b": {
        "name": "FLUX.2 Klein 9B",
        "repo": "black-forest-labs/FLUX.2-klein-9B",
        "guidance": 0.0,
        "steps": 4,
    },
    "flux-klein-4b": {
        "name": "FLUX.2 Klein 4B",
        "repo": "black-forest-labs/FLUX.2-klein-4B",
        "guidance": 0.0,
        "steps": 4,
    },
}

# 9:16 portrait — generate small, upscale 2x after
WIDTH = 432
HEIGHT = 768


def load_pipeline(model_key: str):
    import torch

    model_cfg = MODELS[model_key]
    device = "cuda" if torch.cuda.is_available() else "cpu"
    dtype = torch.bfloat16 if device == "cuda" else torch.float32

    # Enable flash SDP attention via PyTorch 2+ (automatic when flash_attn is installed)
    has_flash = False
    try:
        import flash_attn  # noqa: F401
        has_flash = True
    except ImportError:
        pass

    load_kwargs = {"torch_dtype": dtype}

    print(f"Loading {model_cfg['name']} from {model_cfg['repo']}...")
    repo = model_cfg["repo"]

    if "klein" in repo.lower():
        from diffusers import Flux2KleinPipeline
        pipe = Flux2KleinPipeline.from_pretrained(repo, **load_kwargs)
        pipe.enable_model_cpu_offload()
    elif "bnb-4bit" in repo:
        # Pre-quantized 4-bit — load components to CPU first, then offload
        from diffusers import Flux2Pipeline, Flux2Transformer2DModel
        from transformers import Mistral3ForConditionalGeneration

        print("  Loading text encoder to CPU...")
        text_encoder = Mistral3ForConditionalGeneration.from_pretrained(
            repo, subfolder="text_encoder", torch_dtype=dtype, device_map="cpu"
        )
        print("  Loading transformer to CPU...")
        transformer = Flux2Transformer2DModel.from_pretrained(
            repo, subfolder="transformer", torch_dtype=dtype, device_map="cpu"
        )
        print("  Assembling pipeline...")
        pipe = Flux2Pipeline.from_pretrained(
            repo, transformer=transformer, text_encoder=text_encoder, torch_dtype=dtype
        )
        pipe.enable_model_cpu_offload()
    elif "FLUX.2" in repo:
        # Full FLUX.2-dev — quantize transformer to fp8 on the fly
        from diffusers import Flux2Pipeline, Flux2Transformer2DModel
        from optimum.quanto import freeze, qfloat8, quantize

        print("  Loading transformer...")
        transformer = Flux2Transformer2DModel.from_pretrained(
            repo, subfolder="transformer", torch_dtype=dtype
        )
        print("  Quantizing transformer to FP8...")
        quantize(transformer, weights=qfloat8)
        freeze(transformer)

        print("  Loading pipeline...")
        pipe = Flux2Pipeline.from_pretrained(
            repo, transformer=None, torch_dtype=dtype
        )
        pipe.transformer = transformer
        pipe.enable_model_cpu_offload()
    else:
        from diffusers import FluxPipeline
        pipe = FluxPipeline.from_pretrained(repo, **load_kwargs).to("cuda")

    attn_str = " (flash_attn available)" if has_flash else ""
    print(f"  Ready{attn_str}")

    return pipe, model_cfg


def build_combos(races_json: dict, classes_json: dict):
    """Build all Race × Class × Gender combos with prompts."""
    race_names = [r["Name"] for r in races_json.values()]
    class_names = [c["Name"] for c in classes_json.values()]

    combos = []
    for race_name in race_names:
        if race_name not in RACES:
            continue
        race_data = RACES[race_name]
        for class_name in class_names:
            if class_name not in CLASSES:
                continue
            class_data = CLASSES[class_name]
            for gender in GENDERS:
                prompt = build_prompt(race_name, race_data, class_name, class_data, gender)
                key = f"{race_name}_{class_name}_{gender}".lower().replace(" ", "_").replace("-", "_")
                combos.append({
                    "key": key,
                    "race": race_name,
                    "class": class_name,
                    "gender": gender,
                    "prompt": prompt,
                })
    return combos


def main():
    parser = argparse.ArgumentParser(description="Generate character portraits")
    parser.add_argument("--model", default="flux-dev", choices=list(MODELS.keys()))
    parser.add_argument("--limit", type=int, default=0, help="Generate only first N (0=all)")
    parser.add_argument("--skip-existing", action="store_true", help="Skip already generated")
    parser.add_argument("--seed", type=int, default=42, help="Base seed for reproducibility")
    args = parser.parse_args()

    base = Path(__file__).parent.parent
    races = json.loads((base / "dist" / "gamedata" / "races.json").read_text())
    classes = json.loads((base / "dist" / "gamedata" / "classes.json").read_text())

    combos = build_combos(races, classes)
    print(f"Total combos: {len(combos)}")

    outdir = base / "dist" / "portraits"
    outdir.mkdir(parents=True, exist_ok=True)

    # Filter
    if args.skip_existing:
        combos = [c for c in combos if not (outdir / f"{c['key']}.webp").exists()]
        print(f"After skipping existing: {len(combos)} remaining")

    if args.limit > 0:
        combos = combos[:args.limit]

    if not combos:
        print("Nothing to generate!")
        return

    print(f"Generating {len(combos)} portraits at {WIDTH}x{HEIGHT} with {args.model}")
    print()

    # Show what we'll generate
    for i, c in enumerate(combos):
        print(f"  [{i+1}] {c['key']}")
    print()

    import torch
    pipe, model_cfg = load_pipeline(args.model)

    generated = 0
    errors = 0
    t0 = time.time()

    for i, combo in enumerate(combos):
        outpath = outdir / f"{combo['key']}.webp"
        print(f"[{i+1}/{len(combos)}] {combo['key']}")
        print(f"  Prompt: {combo['prompt'][:120]}...")

        try:
            seed = args.seed + i
            generator = torch.Generator(device="cpu").manual_seed(seed)

            with torch.no_grad():
                image = pipe(
                    prompt=combo["prompt"],
                    width=WIDTH,
                    height=HEIGHT,
                    num_inference_steps=model_cfg["steps"],
                    guidance_scale=model_cfg["guidance"],
                    generator=generator,
                ).images[0]

            buf = io.BytesIO()
            image.save(buf, format="WebP", quality=92)
            outpath.write_bytes(buf.getvalue())
            size_kb = len(buf.getvalue()) / 1024
            generated += 1
            print(f"  OK ({size_kb:.0f} KB)")

        except Exception as e:
            errors += 1
            print(f"  ERROR: {e}")

    # Cleanup
    del pipe
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    elapsed = time.time() - t0
    print(f"\nDone: {generated} generated, {errors} errors in {elapsed:.0f}s")
    print(f"  Avg: {elapsed/max(generated,1):.1f}s per image")
    print(f"  Output: {outdir}")


if __name__ == "__main__":
    main()
