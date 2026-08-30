#!/usr/bin/env python3
"""Protocol and planted-negative tests for the executable A3 adapter envelope."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
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
    source_role = "standalone" if role == "pulp-standalone" else role
    root = Path(r["artifact_directory"])
    sources = {{
        "health_result": SOURCE / f"{{source_role}}-health.json",
        "raw_cold": SOURCE / f"{{source_role}}-cold.json",
        "raw_warm": SOURCE / f"{{source_role}}-warm.json",
        "product_artifact": SOURCE / f"{{source_role}}-product.bin",
        "host_artifact": SOURCE / f"{{source_role}}-host.bin",
        "trace": SOURCE / f"{{source_role}}-trace.pftrace",
        "trace_analysis": SOURCE / f"{{source_role}}-trace-analysis.json",
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
    control = "blank-negative" if kind == "blank" else "audio-thread-exclusion"
    spec = fixture.a3.CONTROL_SPECS[control]
    source_blob = fixture.a2t.git_blobs(
        fixture.ROOT, fixture.PULP_REVISION, {spec["source_path"]},
    )[spec["source_path"]]
    build = {
        "schema": "pulp.gpu-first-visible-control-build-identity.v1",
        "version": 1,
        "target": spec["target"],
        "source_path": spec["source_path"],
        "source_revision": fixture.PULP_REVISION,
        "source_blob": source_blob,
        "build_id": "b" * 64 + "-Test",
        "build_config": "Test",
        "configured_at_utc": "2026-08-29T12:00:00Z",
        "git_dirty": False,
    }
    receipt = json.loads(
        (evidence / ("blank.json" if kind == "blank" else "audio.json")).read_text()
    )
    receipt["control_build"] = build
    environment = (
        "PULP_A3_BLANK_NEGATIVE_RECEIPT_PATH"
        if kind == "blank"
        else "PULP_A3_AUDIO_THREAD_EXCLUSION_RECEIPT_PATH"
    )
    payload = json.dumps(build, sort_keys=True, separators=(",", ":")).encode()
    marker = (
        fixture.a3.CONTROL_MARKER_PREFIX + payload + fixture.a3.CONTROL_MARKER_SUFFIX
    )
    source = path.with_suffix(".c")
    source.write_text(
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "__attribute__((used)) static const unsigned char marker[] = {"
        + ",".join(str(byte) for byte in marker) + "};\n"
        + f"static const char receipt[] = {json.dumps(json.dumps(receipt, sort_keys=True) + chr(10))};\n"
        + "int main(void) {\n"
        + f"  const char *destination = getenv({json.dumps(environment)});\n"
        + f"  const char *mutation = getenv(\"PULP_A3_TEST_CONTROL_MUTATION\");\n"
        + f"  if (mutation && strcmp(mutation, {json.dumps(kind)}) == 0) return 1;\n"
        + "  if (!destination) return 64; FILE *f = fopen(destination, \"wb\");\n"
        + "  if (!f) return 65; size_t n = fwrite(receipt, 1, sizeof(receipt) - 1, f);\n"
        + "  return fclose(f) == 0 && n == sizeof(receipt) - 1 ? 0 : 66;\n}\n",
        encoding="utf-8",
    )
    compiler = shutil.which("cc")
    assert compiler is not None
    subprocess.run([compiler, str(source), "-o", str(path)], check=True)
    path.chmod(0o755)


def write_fake_control(path: Path, evidence: Path, kind: str) -> None:
    source = evidence / ("blank.json" if kind == "blank" else "audio.json")
    environment = (
        "PULP_A3_BLANK_NEGATIVE_RECEIPT_PATH"
        if kind == "blank"
        else "PULP_A3_AUDIO_THREAD_EXCLUSION_RECEIPT_PATH"
    )
    path.write_text(
        "#!/usr/bin/env python3\nimport os, shutil\n"
        f"shutil.copyfile({str(source)!r}, os.environ[{environment!r}])\n",
        encoding="utf-8",
    )
    path.chmod(0o755)


def run_case(
    root: Path, evidence: Path, identity: Path, producer: Path,
    blank: Path, audio: Path, label: str, *, controls: bool,
    producer_mutation: str = "", control_mutation: str = "",
    configure_producer: bool = True, configure_controls: bool = True,
    pulp_root: Path | None = None,
    control_source_root: Path | None = None,
) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
    output = root / f"run-{label}"
    command = [
        sys.executable, str(RUNNER), "run-role", "--role", "pulp-standalone",
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
        "PULP_A3_TEST_CONTROL_MUTATION", "PULP_A3_PULP_ROOT",
        "PULP_A3_ROLE_PRODUCER_SUPPORT",
        "PULP_A3_CONTROL_SOURCE_ROOT",
    ):
        environment.pop(name, None)
    if configure_producer:
        # Preserve a planted symlink spelling so the adapter, not this helper,
        # is the component that proves configured executables are non-symlinks.
        environment["PULP_A3_CAMPAIGN_PRODUCER"] = str(producer.absolute())
    if pulp_root is not None:
        environment["PULP_A3_PULP_ROOT"] = str(pulp_root.resolve())
    if controls and configure_controls:
        environment["PULP_A3_BLANK_CONTROL_BIN"] = str(blank.resolve())
        environment["PULP_A3_AUDIO_CONTROL_BIN"] = str(audio.resolve())
        environment["PULP_A3_CONTROL_SOURCE_ROOT"] = str(
            (control_source_root or fixture.ROOT).resolve()
        )
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
        identity = copy.deepcopy(receipt["campaigns"][0]["identity"])
        identity["forge_revision"] = None
        write_json(identity_path, identity)
        producer = root / "real-product-producer.py"
        blank = root / "blank-control.py"
        audio = root / "audio-control.py"
        write_producer(producer, evidence)
        write_control(blank, evidence, "blank")
        write_control(audio, evidence, "audio")
        # Other CTests deliberately mutate the shared checkout. Validate the
        # strict clean-source control contract in an exact-head private clone,
        # so parallel scheduling cannot make this self-test observe their
        # temporary dirt while the production adapter remains fail-closed.
        control_source_root = root / "clean-control-source"
        subprocess.run(
            [
                "git", "clone", "--quiet", "--no-local", "--depth=1",
                f"file://{fixture.ROOT.resolve()}", str(control_source_root),
            ],
            check=True,
        )
        clone_head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=control_source_root, text=True,
        ).strip()
        assert clone_head == fixture.PULP_REVISION

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "pass-controls", controls=True,
            control_source_root=control_source_root,
        )
        assert completed.returncode == 0, (completed.stdout, completed.stderr, result)
        assert result["status"] == "pass"
        assert result["measurement_producer"]["sha256"] == digest(producer)
        assert result["controls"]["blank_control_binary"]["sha256"] == digest(blank)
        assert result["controls"]["audio_control_binary"]["sha256"] == digest(audio)
        assert result["controls"]["blank_control_provenance"] is not None
        assert result["controls"]["audio_control_provenance"] is not None

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
        assert result["dependencies"] == ["campaign-producer:pulp-standalone"]

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
            control_source_root=control_source_root,
        )
        assert completed.returncode == 2
        assert result["status"] == "incomplete"
        assert result["dependencies"] == ["control:source-root"]

        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "failed-audio-control", controls=True, control_mutation="audio",
            control_source_root=control_source_root,
        )
        assert completed.returncode == 1
        assert result["status"] == "fail"
        assert "audio-thread-exclusion-control exited 1" in result["reason"]

        fake_blank = root / "copy-only-blank.py"
        fake_audio = root / "copy-only-audio.py"
        write_fake_control(fake_blank, evidence, "blank")
        write_fake_control(fake_audio, evidence, "audio")
        completed, result = run_case(
            root, evidence, identity_path, producer, fake_blank, fake_audio,
            "copy-only-controls", controls=True,
            control_source_root=control_source_root,
        )
        assert completed.returncode == 1
        assert result["status"] == "fail"
        assert "not a native compiled executable" in result["reason"]

        symlink = root / "producer-symlink"
        symlink.symlink_to(producer)
        completed, result = run_case(
            root, evidence, identity_path, symlink, blank, audio,
            "symlink-producer", controls=False,
        )
        assert completed.returncode == 2
        assert result["status"] == "incomplete"
        assert "non-symlink" in result["reason"]

        fake_pulp_root = root / "pulp-root"
        fake_pulp_root.mkdir()
        completed, result = run_case(
            root, evidence, identity_path, producer, blank, audio,
            "off-source-producer", controls=False, pulp_root=fake_pulp_root,
        )
        assert completed.returncode == 1
        assert result["status"] == "fail"
        assert "checked-in role entry point" in result["reason"]

    print("gpu-first-visible-a3-external-adapter: positive=2 planted_negatives=10")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
