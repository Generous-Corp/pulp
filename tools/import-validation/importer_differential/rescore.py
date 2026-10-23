"""Offline rescore of a finished differential corpus.

`compare` runs the importer twice and the observer once, then scores the
artifacts. Rescoring replays a corpus's stored artifacts through the same
`compare_one` instead: a replay executable stands in for the importer and the
observer and copies each lane's recorded output into place, so every
verification, registration, and scoring step runs exactly as it does live,
with no browser and no importer build. Two rescores of one corpus are
byte-identical exactly when the scoring code computes the same report, which
is what makes a scoring refactor provable.

The receipt drops only what the replay cannot reproduce: process timings and
their budget verdicts, observability timings, and the digests of the replay
executables standing in for the importer and observer.
"""

from __future__ import annotations

import hashlib
import json
import os
import shlex
import shutil
import sys
from pathlib import Path
from typing import Any

from .core import LabError, compare_one, write_json

RESCORE_SCHEMA = "pulp-importer-differential-rescore-v1"
FIXTURE_ENV = "PULP_IMPORTER_RESCORE_FIXTURE"
LANES = {"importer-browser": "browser", "importer-native": "candidate"}
NONREPRODUCIBLE_REPORT_KEYS = ("timings", "timing_budget", "observability")
NONREPRODUCIBLE_INTEGRITY_KEYS = ("importer_sha256", "observer_sha256")


def option(argv: list[str], name: str) -> Path:
    try:
        return Path(argv[argv.index(name) + 1])
    except (ValueError, IndexError) as exc:
        raise LabError(f"replay invocation has no {name}") from exc


def replay(role: str, argv: list[str]) -> int:
    """Stand in for one recorded process by copying its stored outputs."""
    fixture = Path(os.environ[FIXTURE_ENV])
    if role == "observer":
        recorded = fixture / "candidate"
        for flag, name in (("--render", "render.png"), ("--layout", "layout.json"),
                           ("--render-receipt", "render-receipt.json")):
            shutil.copy2(recorded / name, option(argv, flag))
        return 0
    lane = "importer-native" if "--native-panel-lowering" in argv else "importer-browser"
    recorded = fixture / LANES[lane]
    output = option(argv, "--output")
    shutil.copy2(recorded / "design.ir.json", output)
    evidence = output.with_name(output.stem + "-browser-capture")
    if evidence.exists():
        shutil.rmtree(evidence)
    shutil.copytree(recorded / "design.ir-browser-capture", evidence)
    return 0


def replay_executable(directory: Path, role: str) -> Path:
    entry = Path(__file__).resolve().parents[1] / "importer_differential_lab.py"
    path = directory / f"replay-{role}"
    path.write_text(
        "#!/bin/sh\n"
        f"exec {shlex.quote(sys.executable)} {shlex.quote(str(entry))} "
        f"replay-artifact {role} \"$@\"\n", encoding="utf-8")
    path.chmod(0o755)
    return path


def reproducible_report(report: dict[str, Any]) -> dict[str, Any]:
    result = {key: value for key, value in report.items()
              if key not in NONREPRODUCIBLE_REPORT_KEYS}
    result["integrity"] = {
        key: value for key, value in report["integrity"].items()
        if key not in NONREPRODUCIBLE_INTEGRITY_KEYS}
    return result


def tree_digests(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix():
            hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*")) if path.is_file()}


def rescore_corpus(corpus: Path, output: Path, fixture_sources: Path) -> dict[str, Any]:
    """Rescore every completed fixture under `corpus/fixtures`.

    `fixture_sources` is the directory that holds interaction plans named by
    fixture metadata; each source is replayed from the corpus's own copy, so
    its digest is the one the stored captures were bound to.
    """
    output.mkdir(parents=True, exist_ok=True)
    tools = output / "replay-bin"
    tools.mkdir(exist_ok=True)
    importer = replay_executable(tools, "importer")
    observer = replay_executable(tools, "observer")
    fixtures: dict[str, Any] = {}
    skipped: list[str] = []
    for fixture in sorted((corpus / "fixtures").iterdir()):
        stored = fixture / "comparison" / "report.json"
        if not stored.is_file():
            skipped.append(fixture.name)
            continue
        metadata = json.loads(stored.read_text(encoding="utf-8"))["fixture"]
        sources = list((fixture / "source").iterdir())
        if len(sources) != 1:
            raise LabError(f"{fixture.name}: expected one stored source")
        inputs = output / "inputs" / fixture.name
        inputs.mkdir(parents=True, exist_ok=True)
        source = inputs / sources[0].name
        shutil.copy2(sources[0], source)
        plan = (metadata or {}).get("browser_interactions")
        if plan:
            shutil.copy2(fixture_sources / plan, inputs / plan)
        target = output / "fixtures" / fixture.name
        if target.exists():
            shutil.rmtree(target)
        os.environ[FIXTURE_ENV] = str(fixture)
        try:
            report = compare_one(
                importer, observer, source, target, 60,
                (metadata or {}).get("from", "claude"), None, metadata,
                (metadata or {}).get("cache_state", "unknown"), None)
        finally:
            os.environ.pop(FIXTURE_ENV, None)
        reproducible = reproducible_report(report)
        # Emitted key order is part of the report a consumer diffs, so the
        # digest keeps it rather than sorting it away.
        canonical = json.dumps(reproducible, separators=(",", ":"))
        fixtures[fixture.name] = {
            "report_sha256": hashlib.sha256(canonical.encode()).hexdigest(),
            "overlays": tree_digests(target / "comparison" / "overlays"),
            "report": reproducible,
        }
    receipt = {"schema": RESCORE_SCHEMA, "fixtures": fixtures,
               "skipped_without_report": skipped}
    write_json(output / "rescore.json", receipt)
    return receipt
