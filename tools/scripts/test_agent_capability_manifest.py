#!/usr/bin/env python3
"""Negative and compatibility tests for the agent capability manifest."""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/scripts/agent_capability_manifest.py"
FIXTURES = ROOT / "test/fixtures/agent-capabilities"


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(TOOL), *args], cwd=ROOT, text=True,
                          capture_output=True)


def expect_failure(name: str, needle: str) -> None:
    result = run("--validate", str(FIXTURES / name))
    assert result.returncode != 0, f"{name} unexpectedly passed"
    assert needle in result.stderr, f"{name}: expected {needle!r} in {result.stderr!r}"


def main() -> int:
    expect_failure("duplicate-key.json", "duplicate capability key")
    expect_failure("missing-symbol.json", "outside curated exports")
    expect_failure("missing-descriptor.json", "references missing Forge descriptor")

    canonical = json.loads(run("--json").stdout)
    with tempfile.TemporaryDirectory() as temp:
        stale = pathlib.Path(temp) / "agent-capabilities.json"
        stale_doc = dict(canonical)
        stale_doc["capabilities"] = canonical["capabilities"][:-1]
        stale.write_text(json.dumps(stale_doc, indent=2) + "\n")
        result = run("--check", "--snapshot", str(stale))
        assert result.returncode != 0 and "STALE" in result.stderr, result.stderr

    legacy = subprocess.run([sys.executable, str(ROOT / "tools/dsp_vocabulary.py"), "--json"],
                            cwd=ROOT, text=True, capture_output=True, check=True)
    vocabulary = json.loads(legacy.stdout)
    advertised_signal = [row for row in canonical["capabilities"] if row["domain"] == "signal"]
    for row in advertised_signal:
        relative = row["include"].removeprefix("pulp/signal/")
        assert relative in vocabulary, f"legacy vocabulary lost {relative}"
        class_name = row["symbol"].split("::")[-1].split("<", 1)[0]
        assert any(item["class"] == class_name for item in vocabulary[relative]), class_name

    print("agent-capabilities: 5 negative/compatibility checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
