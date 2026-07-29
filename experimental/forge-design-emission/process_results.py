#!/usr/bin/env python3
"""Extract provider text, validate rounds, translate final passing artifacts."""

import json
import re
from pathlib import Path

from profile_to_design_ir import translate
from validate_emission import validate

ROOT = Path(__file__).parent


def extract_object(text):
    text = re.sub(r"^\s*```(?:json)?\s*", "", text)
    text = re.sub(r"\s*```\s*$", "", text)
    decoder = json.JSONDecoder()
    start = text.find("{")
    if start < 0:
        raise ValueError("no JSON object")
    value, _ = decoder.raw_decode(text[start:])
    return value


def provider_text(provider, case):
    if provider == "claude":
        raw = json.loads((ROOT / "artifacts/raw/claude" / f"{case}.provider.json").read_text())
        return raw["result"]
    return (ROOT / "artifacts/raw/gpt-5.5" / f"{case}.txt").read_text()


def main():
    summary = []
    for provider in ("claude", "gpt-5.5"):
        for concept_path in sorted((ROOT / "concepts").glob("*.json")):
            concept = json.loads(concept_path.read_text())
            for arm in (1, 2):
                case = f"{concept['id']}-arm{arm}"
                out = ROOT / "artifacts" / provider / case
                out.mkdir(parents=True, exist_ok=True)
                raw_text = provider_text(provider, case)
                (out / "round0.txt").write_text(raw_text)
                record = {"provider": provider, "case": case, "round": 0}
                try:
                    artifact = extract_object(raw_text)
                    (out / "round0.json").write_text(json.dumps(artifact, indent=2) + "\n")
                    result = validate(artifact, concept)
                    (out / "round0.validation.json").write_text(json.dumps(result, indent=2) + "\n")
                    record["layers"] = result["layers"]
                    record["diagnostics"] = result["diagnostics"]
                    (out / "final.json").write_text(json.dumps(artifact, indent=2) + "\n")
                    (out / "design_ir.json").write_text(json.dumps(translate(artifact), indent=2) + "\n")
                except Exception as exc:
                    record["layers"] = {"L0": False}
                    record["diagnostics"] = [{"layer": "L0", "path": "", "message": str(exc)}]
                summary.append(record)
    (ROOT / "metrics").mkdir(exist_ok=True)
    (ROOT / "metrics" / "round0.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
