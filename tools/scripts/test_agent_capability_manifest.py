#!/usr/bin/env python3
"""Negative and compatibility tests for the agent capability manifest."""
from __future__ import annotations

import json
import importlib.util
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
    expect_failure("wrong-types.json", "summary must be a non-empty string")
    expect_failure("nested-wrong-types.json", "units must be an array of non-empty strings")
    expect_failure("count-drift.json", "counts must exactly match")
    expect_failure("unknown-field.json", "unknown fields: schedulling")

    canonical = json.loads(run("--json").stdout)
    with tempfile.TemporaryDirectory() as temp:
        stale = pathlib.Path(temp) / "agent-capabilities.json"
        stale_doc = dict(canonical)
        stale_doc["capabilities"] = canonical["capabilities"][:-1]
        stale.write_text(json.dumps(stale_doc, indent=2) + "\n")
        result = run("--check", "--snapshot", str(stale))
        assert result.returncode != 0 and "STALE" in result.stderr, result.stderr

    vocabulary_tool = ROOT / "tools/dsp_vocabulary.py"
    spec = importlib.util.spec_from_file_location("pulp_dsp_vocabulary_test", vocabulary_tool)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    header_projection = module.scan_headers()
    expected_json = json.dumps(header_projection, indent=2) + "\n"
    legacy = subprocess.run([sys.executable, str(vocabulary_tool), "--json"], cwd=ROOT,
                            text=True, capture_output=True, check=True)
    assert legacy.stdout == expected_json, "legacy JSON output changed or bypassed the manifest"
    legacy_markdown = subprocess.run([sys.executable, str(vocabulary_tool)], cwd=ROOT,
                                     text=True, capture_output=True, check=True)
    assert legacy_markdown.stdout == module.markdown(header_projection) + "\n"
    vocabulary = json.loads(legacy.stdout)
    manifest_projection = canonical["compatibility"]["signal_vocabulary"]["entries"]
    assert vocabulary == manifest_projection == header_projection
    advertised_signal = [row for row in canonical["capabilities"] if row["domain"] == "signal"]
    for row in advertised_signal:
        relative = row["include"].removeprefix("pulp/signal/")
        assert relative in vocabulary, f"legacy vocabulary lost {relative}"
        class_name = row["symbol"].split("::")[-1].split("<", 1)[0]
        assert any(item["class"] == class_name for item in vocabulary[relative]), class_name

    print("agent-capabilities: 11 negative/compatibility checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
