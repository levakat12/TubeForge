#!/usr/bin/env python3
"""Local Demucs worker used by TubeForge's offline reconstruction pipeline."""

from __future__ import annotations

import argparse
import importlib.metadata
import json
import math
import os
import sys
import traceback
from pathlib import Path


def write_json(path: Path, payload: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser(description="TubeForge local ML stem-separation worker")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--model", default="htdemucs_6s")
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    try:
        import demucs.api
        import torch
    except Exception as error:
        print(f"Demucs runtime unavailable: {error}", file=sys.stderr)
        return 3

    if args.check:
        print(json.dumps({
            "ready": True,
            "demucsVersion": importlib.metadata.version("demucs"),
            "torchVersion": importlib.metadata.version("torch"),
            "cudaAvailable": bool(torch.cuda.is_available()),
        }))
        return 0

    if not args.input.is_file():
        print(f"Input audio does not exist: {args.input}", file=sys.stderr)
        return 2
    args.output.mkdir(parents=True, exist_ok=True)
    progress_path = args.output / "progress.json"
    selected_device = "cuda" if args.device == "auto" and torch.cuda.is_available() else args.device
    if selected_device == "auto":
        selected_device = "cpu"

    def progress(update: dict) -> None:
        audio_length = max(1, int(update.get("audio_length", 1)))
        offset = max(0, int(update.get("segment_offset", 0)))
        model_count = max(1, int(update.get("models", 1)))
        model_index = max(0, int(update.get("model_idx_in_bag", 0)))
        fraction = min(0.98, (model_index + min(1.0, offset / audio_length)) / model_count)
        write_json(progress_path, {
            "fraction": fraction,
            "stage": f"Demucs {args.model} on {selected_device}: {fraction * 100.0:.0f}%",
        })

    write_json(progress_path, {"fraction": 0.01, "stage": f"Loading Demucs model {args.model}"})
    separator = demucs.api.Separator(
        model=args.model,
        device=selected_device,
        shifts=1,
        split=True,
        overlap=0.25,
        callback=progress,
        progress=False,
    )
    origin, separated = separator.separate_audio_file(str(args.input))
    if not isinstance(separated, dict):
        raise RuntimeError("Demucs returned an unsupported stem collection")

    write_json(progress_path, {"fraction": 0.98, "stage": "Writing neural stems"})
    written: list[str] = []
    for stem in ("vocals", "drums", "bass", "other", "guitar", "piano"):
        source = separated.get(stem)
        if source is None:
            continue
        target = args.output / f"{stem}.wav"
        demucs.api.save_audio(source, str(target), samplerate=separator.samplerate,
                              bits_per_sample=24, clip="rescale")
        written.append(stem)

    if "vocals" not in written or "drums" not in written or "bass" not in written:
        raise RuntimeError(f"Model did not return required stems: {written}")

    residual_ratio = 0.0
    if written:
        summed = None
        for stem in written:
            value = separated[stem]
            summed = value.clone() if summed is None else summed + value
        if summed is not None and tuple(summed.shape) == tuple(origin.shape):
            numerator = torch.mean((origin - summed) ** 2).sqrt().item()
            denominator = max(1.0e-12, torch.mean(origin ** 2).sqrt().item())
            residual_ratio = float(numerator / denominator)
            if not math.isfinite(residual_ratio):
                residual_ratio = 0.0

    manifest = {
        "schemaVersion": 1,
        "backend": "demucs",
        "model": args.model,
        "modelVersion": f"demucs-{importlib.metadata.version('demucs')}:{args.model}",
        "torchVersion": importlib.metadata.version("torch"),
        "device": selected_device,
        "sampleRate": separator.samplerate,
        "stems": written,
        "reconstructionError": residual_ratio,
    }
    write_json(args.output / "manifest.json", manifest)
    write_json(progress_path, {"fraction": 1.0, "stage": "Neural separation complete"})
    print(json.dumps(manifest))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130) from None
    except Exception:
        traceback.print_exc()
        raise SystemExit(1) from None
