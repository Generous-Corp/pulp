#!/usr/bin/env python3
"""Protocol and planted-negative tests for the executable A3 adapter envelope."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
RUNNER = SCRIPT_DIR / "gpu_first_visible_a3_campaign.py"
ADAPTER = SCRIPT_DIR / "gpu_first_visible_a3_external_adapter.py"
sys.path.insert(0, str(SCRIPT_DIR))
import test_gpu_first_visible_a3_acceptance as fixture  # noqa: E402


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_producer(path: Path, evidence: Path) -> None:
    path.write_text(
        f'''#!/usr/bin/env python3
import argparse, hashlib, json, os, shutil, sys
from pathlib import Path

SOURCE = Path({str(evidence)!r})
p = argparse.ArgumentParser()
p.add_argument("--request", required=True, type=Path)
p.add_argument("--receipt", required=True, type=Path)
a = p.parse_args()
r = json.loads(a.request.read_text())
mutation = os.environ.get("PULP_A3_TEST_PRODUCER_MUTATION", "")
outcome = "inconclusive" if mutation == "inconclusive" else "pass"
reason = "real product driver unavailable" if outcome != "pass" else None
dependencies = ["product:test-unavailable"] if outcome != "pass" else []
artifacts = {{key: None for key in (
    "health_result", "raw_cold", "raw_warm", "product_artifact",
    "host_artifact", "trace", "trace_analysis")}}
if outcome == "pass":
    role = r["role"]
    root = Path(r["artifact_directory"])
    sources = {{
        "health_result": SOURCE / f"{{role}}-health.json",
        "raw_cold": SOURCE / f"{{role}}-cold.json",
        "raw_warm": SOURCE / f"{{role}}-warm.json",
        "product_artifact": SOURCE / f"{{role}}-product.bin",
        "host_artifact": SOURCE / f"{{role}}-host.bin",
        "trace": SOURCE / f"{{role}}-trace.pftrace",
        "trace_analysis": SOURCE / f"{{role}}-trace-analysis.json",
    }}
    for name, source in sources.items():
        if mutation == "missing-core" and name == "trace":
            continue
        destination = root / "producer" / f"{{name}}{{source.suffix}}"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        artifacts[name] = {{
            "path": destination.relative_to(a.request.parent).as_posix(),
            "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
        }}
receipt = {{
    "schema": (
        "pulp.gpu-first-visible-campaign-producer.invalid"
        if mutation == "wrong-schema"
        else "pulp.gpu-first-visible-campaign-producer.v1"
    ),
    "version": 1,
    "attempt_nonce": r["attempt_nonce"],
    "role": r["role"],
    "outcome": outcome,
    "reason": reason,
    "dependencies": dependencies,
    "identity": r["identity"],
    "measurement_endpoint": r["measurement_endpoint"],
    "artifacts": artifacts,
}}
a.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\\n")
if mutation == "exit-mismatch":
    raise SystemExit(1)
raise SystemExit({{"pass": 0, "inconclusive": 2}}[outcome])
''',
        encoding="utf-8",
    )
    path.chmod(0o755)


def write_control(path: Path, evidence: Path, kind: str) -> None:
    source = evidence / ("blank.json" if kind == "blank" else "audio.json")
    environment = (
        "PULP_A3_BLANK_NEGATIVE_RECEIPT_PATH"
        if kind == "blank"
        else "PULP_A3_AUDIO_THREAD_EXCLUSION_RECEIPT_PATH"
    )
    path.write_text(
        f'''#!/usr/bin/env python3
import os, shutil, sys
from pathlib import Path
source = Path({str(source)!r})
destination = os.environ.get({environment!r})
if not destination:
    raise SystemExit(64)
if os.environ.get("PULP_A3_TEST_CONTROL_MUTATION") == {kind!r}:
    raise SystemExit(1)
Path(destination).parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(source, destination)
raise SystemExit(0)
''',
        encoding="utf-8",
    )
    path.chmod(0o755)


def run_case(
    root: Path, evidence: Path, identity: Path, producer: Path,
    blank: Path, audio: Path, label: str, *, controls: bool,
    producer_mutation: str = "", control_mutation: str = "",
    configure_producer: bool = True, configure_controls: bool = True,
) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
    output = root / f"run-{label}"
    command = [
        sys.executable, str(RUNNER), "run-role", "--role", "standalone",
        "--identity", str(identity),
        "--budget-receipt", str(evidence / "budget.json"),
        "--budget-cold", str(evidence / "budget-cold.json"),
        "--budget-warm", str(evidence / "budget-warm.json"),
        "--adapter", str(ADAPTER), "--output-dir", str(output),
        "--timeout-seconds", "30",
    ]
    if controls:
        command.append("--require-controls")
    environment = dict(os.environ)
    for name in (
        "PULP_A3_CAMPAIGN_PRODUCER", "PULP_A3_BLANK_CONTROL_BIN",
        "PULP_A3_AUDIO_CONTROL_BIN", "PULP_A3_TEST_PRODUCER_MUTATION",
        "PULP_A3_TEST_CONTROL_MUTATION",
    ):
        environment.pop(name, None)
    if configure_producer:
        # Preserve a planted symlink spelling so the adapter, not this helper,
        # is the component that proves configured executables are non-symlinks.
        environment["PULP_A3_CAMPAIGN_PRODUCER"] = str(producer.absolute())
    if controls and configure_controls:
        environment["PULP_A3_BLANK_CONTROL_BIN"] = str(blank.resolve())
        environment["PULP_A3_AUDIO_CONTROL_BIN"] = str(audio.resolve())
    if producer_mutation:
        environment["PULP_A3_TEST_PRODUCER_MUTATION"] = producer_mutation
    if control_mutation:
        environment["PULP_A3_TEST_CONTROL_MUTATION"] = control_mutation
    completed = subprocess.run(
        command, env=environment, text=True, capture_output=True, check=False,
    )
    run_path = output / "run.json"
    result = json.loads(run_path.read_text(encoding="utf-8")) if run_path.is_file() else {}
    return completed, result


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-a3-external-adapter-") as temporary:
        root = Path(temporary)
        evidence = root / "evidence"
        evidence.mkdir()
        receipt = fixture.make_fixture(evidence)
        identity_path = root / "identity.json"
        write_json(identity_path, receipt["campaigns"][0]["identity"])
        producer = root / "real-product-producer.py"
        blank = root / "blank-control.py"
        audio = root / "audio-control.py"
        write_producer(producer, evidence)
        write_control(blank, evidence, "blank")
        write_control(audio, evidence, "audio")

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "pass-controls", controls=True,
        )
        assert completed.returncode == 0, (completed.stdout, completed.stderr, result)
        assert result["status"] == "pass"
        assert result["measurement_producer"]["sha256"] == digest(producer)
        assert result["controls"]["blank_control_binary"]["sha256"] == digest(blank)
        assert result["controls"]["audio_control_binary"]["sha256"] == digest(audio)

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "pass-no-controls", controls=False,
        )
        assert completed.returncode == 0
        assert result["status"] == "pass" and result["controls"] is None

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "missing-producer", controls=False, configure_producer=False,
        )
        assert completed.returncode == 2
        assert result["status"] == "incomplete"
        assert result["dependencies"] == ["campaign-producer:standalone"]

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "producer-inconclusive", controls=False,
            producer_mutation="inconclusive",
        )
        assert completed.returncode == 2
        assert result["status"] == "incomplete"
        assert result["dependencies"] == ["product:test-unavailable"]

        for mutation, needle in (
            ("wrong-schema", "wrong schema"),
            ("missing-core", "omitted trace"),
            ("exit-mismatch", "exit code disagrees"),
        ):
            completed, result = run_case(
                root, evidence, identity_path, producer, blank, audio,
                f"producer-{mutation}", controls=False,
                producer_mutation=mutation,
            )
            assert completed.returncode == 1, (mutation, completed.stderr, result)
            assert result["status"] == "fail"
            assert needle in result["reason"], (mutation, result["reason"])

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "missing-controls", controls=True, configure_controls=False,
        )
        assert completed.returncode == 2
        assert result["status"] == "incomplete"
        assert result["dependencies"] == ["control:blank-negative-binary"]

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "failed-audio-control", controls=True, control_mutation="audio",
        )
        assert completed.returncode == 1
        assert result["status"] == "fail"
        assert "audio-thread-exclusion-control exited 1" in result["reason"]

        symlink = root / "producer-symlink"
        symlink.symlink_to(producer)
        completed, result = run_case(
            root, evidence, identity_path, symlink, blank, audio,
            "symlink-producer", controls=False,
        )
        assert completed.returncode == 2
        assert result["status"] == "incomplete"
        assert "non-symlink" in result["reason"]

    print("gpu-first-visible-a3-external-adapter: positive=2 planted_negatives=8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
