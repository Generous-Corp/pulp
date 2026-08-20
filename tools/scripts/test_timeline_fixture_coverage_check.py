#!/usr/bin/env python3
"""Tests for timeline_fixture_coverage_check.py.

Builds synthetic schema manifests and corpora in a temp directory and asserts
the gate's verdict on each. The key assertion is confirm-the-failure: the check
must PASS on a complete corpus and FAIL (exit 1, naming the version) on an
incomplete one -- and must distinguish a coverage gap (exit 1) from an
operational error (exit 2) -- otherwise the gate proves nothing.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

_SCRIPT = Path(__file__).resolve().with_name("timeline_fixture_coverage_check.py")


def _write_schema(root: Path, current: int, oldest: int = 1) -> Path:
    manifest = {
        "$defs": {
            "pulp.timeline.sequence": {
                "x-pulp-current-version": current,
                "x-pulp-migrations": {
                    "upgrades": [{"from": v, "to": v + 1} for v in range(oldest, current)],
                    "downgrades": [{"from": v + 1, "to": v} for v in range(oldest, current)],
                },
            }
        }
    }
    path = root / "timeline_schema.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path


def _write_corpus(root: Path, entries: list[str], name: str = "corpus") -> Path:
    corpus = root / name
    corpus.mkdir(parents=True, exist_ok=True)
    (corpus / "corpus.index").write_text("\n".join(entries) + "\n", encoding="utf-8")
    return corpus


def _run(schema: Path, corpus: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [sys.executable, str(_SCRIPT), "--schema", str(schema), "--corpus", str(corpus)],
        capture_output=True,
        text=True,
    )
    return proc.returncode, proc.stderr + proc.stdout


def main() -> int:
    failures: list[str] = []

    def check(name: str, condition: bool) -> None:
        if not condition:
            failures.append(name)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        # Complete coverage: every version in the chain has a document fixture.
        schema = _write_schema(root, current=3)
        corpus = _write_corpus(root, [
            "document v1/minimal.json",
            "document v2/device-chain.json",
            "document v3/automation-lane.json",
            "fragment v2/entity.json",
            "payload v1/blob.json",
        ])
        rc, out = _run(schema, corpus)
        check("complete corpus passes", rc == 0)

        # The gap this gate exists for: v5 missing from a v1..v7 chain.
        schema = _write_schema(root, current=7)
        corpus = _write_corpus(root, [
            "document v1/a.json",
            "document v2/a.json",
            "document v3/a.json",
            "document v4/a.json",
            "document v6/a.json",
            "document v7/a.json",
        ])
        rc, out = _run(schema, corpus)
        check("missing middle version fails", rc == 1)
        check("failure names the missing version", "v5" in out)

        # A bump without a fixture: current version itself uncovered.
        schema = _write_schema(root, current=8)
        rc, out = _run(schema, corpus)
        check("bump without fixture fails", rc == 1)
        check("failure names the new version", "v8" in out)

        # A version dir with only non-document kinds is not coverage.
        schema = _write_schema(root, current=2)
        corpus = _write_corpus(root, [
            "document v1/a.json",
            "fragment v2/entity.json",
            "payload v2/blob.json",
        ])
        rc, out = _run(schema, corpus)
        check("non-document kinds do not count", rc == 1)
        check("non-document failure names v2", "v2" in out)

        # An unindexed fixture is dead weight: only corpus.index speaks.
        corpus = _write_corpus(root, ["document v1/a.json"])
        (corpus / "v2").mkdir()
        (corpus / "v2" / "sneaky.json").write_text("{}", encoding="utf-8")
        rc, out = _run(schema, corpus)
        check("unindexed file does not count", rc == 1)
        check("unindexed failure names v2", "v2" in out)

        # Operational errors are exit 2, never a coverage verdict.
        missing_schema = root / "no-such-schema.json"
        rc, out = _run(missing_schema, corpus)
        check("unreadable schema is exit 2", rc == 2)

        corpus_no_index = root / "empty-corpus"
        corpus_no_index.mkdir()
        rc, out = _run(schema, corpus_no_index)
        check("missing corpus.index is exit 2", rc == 2)

        bad_schema = root / "bad_schema.json"
        bad_schema.write_text(json.dumps({"$defs": {}}), encoding="utf-8")
        rc, out = _run(bad_schema, corpus)
        check("manifest without sequence type is exit 2", rc == 2)

        malformed = _write_corpus(root / "other", ["document"])
        rc, out = _run(schema, malformed)
        check("malformed index line is exit 2", rc == 2)

        # A manifest whose chain cannot be trusted must fail closed, never
        # relax the required set.
        no_edges = root / "no_edges.json"
        no_edges.write_text(json.dumps({"$defs": {"pulp.timeline.sequence": {
            "x-pulp-current-version": 5, "x-pulp-migrations": {"upgrades": []}}}}),
            encoding="utf-8")
        rc, out = _run(no_edges, corpus)
        check("current>1 with no migration edges is exit 2", rc == 2)

        broken_chain = root / "broken_chain.json"
        broken_chain.write_text(json.dumps({"$defs": {"pulp.timeline.sequence": {
            "x-pulp-current-version": 5,
            "x-pulp-migrations": {"upgrades": [{"from": 1, "to": 2}, {"from": 2, "to": 3}]}}}}),
            encoding="utf-8")
        rc, out = _run(broken_chain, corpus)
        check("discontiguous chain is exit 2", rc == 2)

        # A v1-only schema legitimately declares no edges.
        v1_only = root / "v1_only.json"
        v1_only.write_text(json.dumps({"$defs": {"pulp.timeline.sequence": {
            "x-pulp-current-version": 1, "x-pulp-migrations": {"upgrades": []}}}}),
            encoding="utf-8")
        v1_corpus = _write_corpus(root / "v1only", ["document v1/a.json"])
        rc, out = _run(v1_only, v1_corpus)
        check("v1-only schema with fixture passes", rc == 0)

    if failures:
        for name in failures:
            print(f"FAIL: {name}", file=sys.stderr)
        return 1
    print("timeline-fixture-coverage selftest: all cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
