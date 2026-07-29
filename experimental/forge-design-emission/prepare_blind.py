#!/usr/bin/env python3
"""Create deterministic anonymous image sets and keep the key out of picker prompts."""

import json
import random
import shutil
from pathlib import Path

ROOT = Path(__file__).parent


def main():
    random.seed(20260728)
    key = {}
    for concept in ("compressor", "synth", "utility"):
        directory = ROOT / "blind" / concept
        directory.mkdir(parents=True, exist_ok=True)
        candidates = [
            (provider, arm, ROOT / "artifacts" / provider / f"{concept}-arm{arm}" / "render.png")
            for provider in ("claude", "gpt-5.5") for arm in (1, 2)
        ]
        random.shuffle(candidates)
        key[concept] = {}
        for label, (provider, arm, source) in zip("ABCD", candidates):
            shutil.copyfile(source, directory / f"{label}.png")
            key[concept][label] = {"provider": provider, "arm": arm}
        shutil.copyfile(ROOT / "references" / "template-floor.png", directory / "FLOOR.png")
        shutil.copyfile(ROOT / "references" / "halo-ceiling.png", directory / "CEILING.png")
    (ROOT / "blind" / "key.json").write_text(json.dumps(key, indent=2) + "\n")


if __name__ == "__main__":
    main()
