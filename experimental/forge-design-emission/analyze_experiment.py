#!/usr/bin/env python3
"""Aggregate pass rates, repair convergence, blind picks and composition variance."""

import json
import subprocess
from itertools import combinations
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).parent


def image_vector(path):
    image = Image.open(path).convert("RGB").resize((256, 256))
    return np.asarray(image, dtype=np.float32) / 255.0


def read_json_text(path):
    text = path.read_text().strip()
    if text.startswith("```"):
        text = text.split("\n", 1)[1].rsplit("```", 1)[0]
    return json.loads(text)


def usage_from_events(path):
    events = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    thread = next((e.get("thread_id") for e in events if e["type"] == "thread.started"), None)
    usage = next((e.get("usage") for e in reversed(events) if e["type"] == "turn.completed"), {})
    return thread, usage


def main():
    rows, calls = [], []
    for provider in ("claude", "gpt-5.5"):
        for concept in ("compressor", "synth", "utility"):
            for arm in (1, 2):
                case = f"{concept}-arm{arm}"
                directory = ROOT / "artifacts" / provider / case
                v0 = json.loads((directory / "round0.validation.json").read_text())
                rounds = 0
                final_validation = v0
                if (directory / "round1.validation.json").exists():
                    rounds = 1
                    final_validation = json.loads((directory / "round1.validation.json").read_text())
                artifact = json.loads((directory / "final.json").read_text())
                rows.append({
                    "provider": provider, "concept": concept, "arm": arm,
                    "single_shot": v0["layers"], "repair_rounds": rounds,
                    "final": final_validation["layers"],
                    "node_count": final_validation["node_count"],
                    "width": artifact["design"]["style"]["width"],
                    "height": artifact["design"]["style"]["height"],
                })
                if provider == "claude":
                    meta = json.loads((ROOT / "artifacts/raw/claude" / f"{case}.provider.json").read_text())
                    model = next(iter(meta["modelUsage"].values()))
                    calls.append({
                        "provider": "Anthropic firstParty", "case": case, "round": 0,
                        "requested_model": "fable", "canonical_model": model["canonicalModel"],
                        "session_id": meta["session_id"], "stop_reason": meta["stop_reason"],
                        "input_tokens": model["inputTokens"] + model["cacheReadInputTokens"] + model["cacheCreationInputTokens"],
                        "output_tokens": model["outputTokens"], "cost_usd": model["costUSD"],
                        "cli_version": subprocess.check_output(["claude", "--version"], text=True).strip(),
                    })
                else:
                    event = ROOT / "artifacts/raw/gpt-5.5" / f"{case}.events.jsonl"
                    thread, usage = usage_from_events(event)
                    calls.append({
                        "provider": "OpenAI Codex", "case": case, "round": 0,
                        "requested_model": "gpt-5.5", "thread_id": thread,
                        "usage": usage, "cli_version": subprocess.check_output(["codex", "--version"], text=True).strip(),
                    })
                    if rounds:
                        thread, usage = usage_from_events(directory / "round1.events.jsonl")
                        calls.append({
                            "provider": "OpenAI Codex", "case": case, "round": 1,
                            "requested_model": "gpt-5.5", "thread_id": thread,
                            "usage": usage, "cli_version": subprocess.check_output(["codex", "--version"], text=True).strip(),
                        })
    key = json.loads((ROOT / "blind/key.json").read_text())
    picks = {}
    for concept in ("compressor", "synth", "utility"):
        pick = read_json_text(ROOT / "blind" / concept / "pick.json")
        pick["decoded_ranking"] = [
            {**key[concept][label], "label": label, "score": pick["scores"][label]}
            for label in pick["ranking"]
        ]
        pick["decoded_beats_floor"] = [
            {**key[concept][label], "label": label} for label in pick["beats_floor"]
        ]
        picks[concept] = pick
    variance = {}
    for concept in ("compressor", "synth", "utility"):
        images = {
            f"{provider}-arm{arm}": image_vector(
                ROOT / "artifacts" / provider / f"{concept}-arm{arm}" / "render.png")
            for provider in ("claude", "gpt-5.5") for arm in (1, 2)
        }
        pairs = {}
        for a, b in combinations(images, 2):
            pairs[f"{a} vs {b}"] = round(float(np.mean(np.abs(images[a] - images[b]))), 4)
        variance[concept] = {
            "mean_pairwise_pixel_mad": round(sum(pairs.values()) / len(pairs), 4),
            "pairwise_pixel_mad": pairs,
            "size_variants": len({(r["width"], r["height"]) for r in rows if r["concept"] == concept}),
            "node_count_range": [
                min(r["node_count"] for r in rows if r["concept"] == concept),
                max(r["node_count"] for r in rows if r["concept"] == concept),
            ],
        }
    pass_rates = {}
    for layer in ("L0", "L1", "L2", "L3", "L4"):
        pass_rates[layer] = {
            "single_shot": sum(bool(r["single_shot"].get(layer)) for r in rows),
            "final": sum(bool(r["final"].get(layer)) for r in rows),
            "attempted": len(rows),
        }
    summary = {
        "artifacts_attempted": len(rows), "renders": len(list(ROOT.glob("artifacts/*/*/render.png"))),
        "pass_rates": pass_rates, "rows": rows, "blind_picks": picks,
        "variance": variance,
        "repair": {"needed": sum(r["repair_rounds"] > 0 for r in rows),
                   "converged_round1": sum(r["repair_rounds"] == 1 and r["final"]["L4"] for r in rows),
                   "needed_round2": 0,
                   "cluster": "style.background contained linear-gradient; use style.gradient"},
    }
    (ROOT / "metrics").mkdir(exist_ok=True)
    (ROOT / "metrics/summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    (ROOT / "calls.jsonl").write_text("".join(json.dumps(call) + "\n" for call in calls))


if __name__ == "__main__":
    main()
