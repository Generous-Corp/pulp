#!/usr/bin/env python3
"""Extract and validate one bounded repair response."""

import argparse
import json
from pathlib import Path

from process_results import extract_object
from profile_to_design_ir import translate
from validate_emission import validate

ROOT = Path(__file__).parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("provider")
    parser.add_argument("case")
    parser.add_argument("round", type=int)
    args = parser.parse_args()
    directory = ROOT / "artifacts" / args.provider / args.case
    concept_id = args.case.rsplit("-arm", 1)[0]
    concept = json.loads((ROOT / "concepts" / f"{concept_id}.json").read_text())
    artifact = extract_object((directory / f"round{args.round}.txt").read_text())
    (directory / f"round{args.round}.json").write_text(json.dumps(artifact, indent=2) + "\n")
    result = validate(artifact, concept)
    (directory / f"round{args.round}.validation.json").write_text(json.dumps(result, indent=2) + "\n")
    if all(result["layers"].get(layer) for layer in ("L0", "L1", "L2", "L3", "L4")):
        (directory / "final.json").write_text(json.dumps(artifact, indent=2) + "\n")
        (directory / "design_ir.json").write_text(json.dumps(translate(artifact), indent=2) + "\n")
    print(args.provider, args.case, json.dumps(result["layers"]), len(result["diagnostics"]))


if __name__ == "__main__":
    main()
