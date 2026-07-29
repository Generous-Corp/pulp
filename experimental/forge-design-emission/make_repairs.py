#!/usr/bin/env python3
"""Build bounded repair prompts from the previous ladder diagnostics."""

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("provider")
    parser.add_argument("case")
    parser.add_argument("round", type=int)
    args = parser.parse_args()
    case_dir = ROOT / "artifacts" / args.provider / args.case
    prior = args.round - 1
    validation = json.loads((case_dir / f"round{prior}.validation.json").read_text())
    original = (ROOT / "prompts" / f"{args.case}.txt").read_text()
    artifact = (case_dir / f"round{prior}.json").read_text()
    diags = validation["diagnostics"][:10]
    prompt = f"""{original}

REPAIR ROUND {args.round} OF 2.
Your previous object failed the validator. Return the COMPLETE corrected object,
again raw JSON only. Preserve good composition; change only what diagnostics
require. In this profile `background` is a solid color/token only. Put any
`linear-gradient(...)` string in the sibling `gradient` field and use a solid
token in `background`.

Diagnostics:
{json.dumps(diags, indent=2)}

Previous object:
{artifact}
"""
    (case_dir / f"round{args.round}.prompt.txt").write_text(prompt)


if __name__ == "__main__":
    main()
