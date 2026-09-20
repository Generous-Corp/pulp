#!/usr/bin/env python3
"""Deterministic protocol and negative tests for the native DPR adapter."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

import gpu_dpr_pulp_native_adapter as native_adapter

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
ADAPTER = SCRIPT_DIR / "gpu_dpr_pulp_native_adapter.py"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


def fake_screenshot(path: Path) -> None:
    path.write_text(
        """#!/usr/bin/env python3
import struct, sys
args = sys.argv[1:]
def value(flag): return args[args.index(flag) + 1]
w, h, scale = int(value('--width')), int(value('--height')), float(value('--scale'))
out = value('--output')
png = b'\\x89PNG\\r\\n\\x1a\\n' + struct.pack('>I', 13) + b'IHDR' + struct.pack('>II', round(w*scale), round(h*scale))
open(out, 'wb').write(png)
print(f'Screenshot saved to {out} ({w}x{h} @{scale:g}x, backend={value("--backend")})')
""",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def fake_measurement_producer(path: Path, *, complete_scope: bool = True) -> None:
    scope_value = "True" if complete_scope else "False"
    path.write_text(
        f'''#!/usr/bin/env python3
import argparse, hashlib, json, os, struct
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--receipt'); a=p.parse_args()
r=json.loads(Path(a.request).read_text()); root=Path(a.receipt).parent
dpr=float(r['requested_dpr'])
if r['mode']=='configured_max': dpr=min(dpr, 2.0)
logical=r['scenario']['logical_size']; w=round(logical['width']*dpr); h=round(logical['height']*dpr)
capture=root/'measured-capture.png'
capture.write_bytes(b'\\x89PNG\\r\\n\\x1a\\n'+struct.pack('>I',13)+b'IHDR'+struct.pack('>II',w,h))
reference=root/'measured-reference.png'; reference.write_bytes(capture.read_bytes())
trace=root/'measured-trace.pftrace'; trace.write_bytes(b'real-perfetto-trace')
producer_digest=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
adapter={{'class':'hardware','name':'Selftest GPU','backend':'Metal','driver':'selftest-1','authentic_identity':True}}
samples=[10.0+i/10 for i in range(20)]
trials=[{{'schema':'pulp.gpu-dpr-first-frame-trial.v1','version':1,
 'attempt_nonce':r['attempt_nonce'],'attempt_number':r['attempt_number'],
 'pid':10000+i,'producer_sha256':producer_digest,
 'content_digest':r['expected_content_digest'],'pulp_sha':r['pulp_sha'],
 'first_frame_time_ms':samples[i],'adapter':adapter}} for i in range(20)]
raw=root/'measured-raw.json'; raw.write_text(json.dumps({{
 'schema':'pulp.gpu-dpr-raw-samples.v1','version':1,'producer_pid':9999,
 'metrics':{{'first_frame_time':{{'provenance':'measured','definition':'fresh process first frame','samples':samples}}}},'fresh_process_trials':trials}})+'\\n')
inputs=root/'measured-input.json'; inputs.write_text(json.dumps({{'logical_input':True}})+'\\n')
def artifact(kind, file): return {{'kind':kind,'path':file.name,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()}}
receipt={{
 'schema':'pulp.gpu-dpr-cell-receipt.v1','version':1,
 'attempt_nonce':r['attempt_nonce'],'attempt_number':r['attempt_number'],
 'scenario_id':r['scenario']['id'],
 'scenario_kind':r['scenario']['kind'],'mode':r['mode'],'requested_dpr':r['requested_dpr'],
 'observed_dpr':dpr,'physical_size':{{'width':w,'height':h}},
 'content_digest':r['expected_content_digest'],'outcome':'pass','reason':None,'dependencies':[],
 'machine':{{'id':'selftest-m3','os':'macos','architecture':'arm64'}},
 'adapter':adapter,
 'build_identity':{{'pulp_sha':r['pulp_sha']}},
 'measurement_scope':{{'schema':'pulp.gpu-dpr-native-measurement-scope.v1',
   'same_process':{{'adapter_identity':True,'capture':True,'frame_metrics':True,
     'memory_metrics':True,'logical_input':True,'trace_correlation':{scope_value}}},
   'audio_device_opened':False}},
 'artifacts':[artifact('capture',capture),artifact('reference_capture',reference),artifact('trace',trace),
              artifact('raw_samples',raw),artifact('input_receipt',inputs)]}}
Path(a.receipt).write_text(json.dumps(receipt)+'\\n')
''',
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def noisy_measurement_producer(path: Path) -> None:
    path.write_text(
        "#!/usr/bin/env python3\n"
        "import os, time\n"
        "os.write(2, b'x' * (1024 * 1024 + 65536))\n"
        "time.sleep(30)\n",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def request(root: Path, expected_digest: str) -> dict:
    return {
        "schema": "pulp.gpu-dpr-cell-request.v1",
        "version": 1,
        "attempt_nonce": "1" * 32,
        "attempt_number": 1,
        "cell_key": "dense-text-thin-strokes__exact__dpr-1.5",
        "scenario": {
            "id": "dense-text-thin-strokes",
            "kind": "pulp_screenshot",
            "source": "dense-text-thin-strokes.ui.js",
            "logical_size": {"width": 640, "height": 360},
            "logical_input_oracle": {"point": [8, 8], "target": "view:root"},
            "fidelity_oracle": {
                "small_text_roi": {"x": 24, "y": 24, "width": 592, "height": 105},
                "thin_stroke_roi": {"x": 24, "y": 155, "width": 592, "height": 145},
            },
            "required_oracles": ["small_text", "thin_strokes", "logical_input"],
        },
        "mode": "exact",
        "requested_dpr": 1.5,
        "expected_content_digest": expected_digest,
        "trial_contract": {
            "fresh_process_first_frame_trials": 20,
            "gpu_timer_calibration_trials": 5,
            "gpu_timer_extra_work_multiplier": 8,
        },
        "pulp_sha": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True
        ).strip(),
        "pulp_source_root": str(root.resolve()),
    }


def main() -> int:
    source = ROOT / "test/fixtures/gpu-ux/dpr/dense-text-thin-strokes.ui.js"
    with tempfile.TemporaryDirectory(prefix="pulp-dpr-native-adapter-") as temporary:
        tmp = Path(temporary)
        executable = tmp / "pulp-screenshot"
        fake_screenshot(executable)
        request_path = tmp / "request.json"
        receipt_path = tmp / "receipt.json"
        document = request(ROOT, digest(source))
        write_json(request_path, document)
        env = dict(os.environ)
        env["PULP_DPR_SCREENSHOT_BIN"] = str(executable)
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 3, completed
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert receipt["outcome"] == "inconclusive"
        assert receipt["attempt_nonce"] == document["attempt_nonce"]
        assert "a2t:correlated-cell-trace" in receipt["dependencies"]
        assert "small-text-legibility-oracle:dense-text-thin-strokes" in receipt["dependencies"]
        preflight = json.loads((tmp / "preflight.json").read_text(encoding="utf-8"))
        assert preflight["physical_size"] == {"width": 960, "height": 540}
        assert preflight["source_sha256"] == digest(source)
        assert preflight["capture_backend_requested"] == "skia"
        assert "gpu_frame_time" in preflight["not_claimed"]

        # Runner-pinned adapters cannot discover the checkout from __file__.
        # The issued request must bind an absolute source root instead.
        document["pulp_source_root"] = "relative/source"
        write_json(request_path, document)
        (tmp / "capture.png").unlink(missing_ok=True)
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 3
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert "source root must be an absolute" in receipt["reason"]
        assert not (tmp / "capture.png").exists()
        document["pulp_source_root"] = str(ROOT.resolve())

        # Negative control: a changed source digest is rejected before capture.
        document["expected_content_digest"] = "0" * 64
        write_json(request_path, document)
        (tmp / "capture.png").unlink(missing_ok=True)
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 3
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert receipt["attempt_nonce"] == document["attempt_nonce"]
        assert "source digest differs" in receipt["reason"]
        assert "native-capture:dense-text-thin-strokes" in receipt["dependencies"]
        assert not (tmp / "capture.png").exists()

        # A measured producer is pinned before execution and may produce a
        # terminal receipt only after attesting every same-process evidence
        # field. The outer runner still validates raw samples and Perfetto.
        document["expected_content_digest"] = digest(source)
        write_json(request_path, document)
        producer = tmp / "native-measurement-producer.exe"
        fake_measurement_producer(producer)
        measured_env = dict(env)
        measured_env["PULP_DPR_NATIVE_MEASUREMENT_BIN"] = str(producer.resolve())
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=measured_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 0, completed
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert receipt["outcome"] == "pass"
        identity = receipt["build_identity"]["measurement_producer"]
        pinned = Path(identity["path"])
        assert pinned.is_file() and not pinned.is_symlink()
        assert pinned.suffix == ".exe"
        assert identity["sha256"] == digest(pinned) == digest(producer)
        attestation = receipt["measurement_attestation"]
        assert attestation["producer_sha256"] == identity["sha256"]
        assert all(attestation["same_process"].values())

        raw_path = tmp / next(
            item["path"] for item in receipt["artifacts"]
            if item["kind"] == "raw_samples"
        )
        valid_raw = json.loads(raw_path.read_text(encoding="utf-8"))
        for label, mutation in [
            ("nonce", ("attempt_nonce", "0" * 32)),
            ("attempt", ("attempt_number", 2)),
            ("digest", ("producer_sha256", "0" * 64)),
            ("pid", ("pid", valid_raw["fresh_process_trials"][0]["pid"])),
        ]:
            planted = json.loads(json.dumps(valid_raw))
            planted["fresh_process_trials"][1][mutation[0]] = mutation[1]
            try:
                native_adapter.validate_fresh_process_ledger(
                    planted, document, receipt, identity["sha256"]
                )
            except ValueError:
                pass
            else:
                raise AssertionError(f"planted mixed {label} ledger passed")
        planted = json.loads(json.dumps(valid_raw))
        planted["fresh_process_trials"][1]["adapter"]["name"] = "Other GPU"
        try:
            native_adapter.validate_fresh_process_ledger(
                planted, document, receipt, identity["sha256"]
            )
        except ValueError:
            pass
        else:
            raise AssertionError("planted mixed adapter ledger passed")

        # Negative control: one missing same-process claim must become a
        # durable inconclusive result, never a terminal pass.
        fake_measurement_producer(producer, complete_scope=False)
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=measured_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 3, completed
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert receipt["outcome"] == "inconclusive"
        assert "did not attest every same-process evidence field" in receipt["reason"]
        assert "native-measurement-producer:dense-text-thin-strokes" in receipt["dependencies"]

        # A noisy product producer is terminated at the one-MiB per-stream
        # boundary and becomes resumable incomplete evidence.
        noisy_measurement_producer(producer)
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=measured_env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 3, completed
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert "output exceeded 1048576 bytes per stream" in receipt["reason"]
        producer_log = tmp / (
            f"measurement-producer-{document['attempt_nonce']}.stderr.log"
        )
        assert producer_log.stat().st_size == 1024 * 1024

        calibration_diagnostics = {
            "schema": "pulp.gpu-dpr-calibration-diagnostics.v1",
            "stage": "calibration",
            "clock": "dawn-gpu-timestamp",
            "attempt_nonce": document["attempt_nonce"],
            "failure_class": "timer_quantization",
            "control_detected": False,
            "reason": "GPU timer did not detect the known-extra-work control",
            "resolution_ms": 0.1,
            "baseline_median_ms": 1.05,
            "extra_work_median_ms": 1.1,
            "delta_ms": 0.05,
            "detection_threshold_ms": 0.20,
            "trials": [
                {"trial": 0,
                 "baseline": {"valid": True, "value_ms": 1.0},
                 "extra": {"valid": True, "value_ms": 1.1}},
                {"trial": 1,
                 "baseline": {"valid": True, "value_ms": 1.0},
                 "extra": {"valid": False, "value_ms": None}},
            ],
        }
        diagnostics_name = (
            f"gpu-timer-calibration-diagnostics-{document['attempt_nonce']}.json"
        )
        diagnostics_file = tmp / diagnostics_name
        diagnostics_file.write_text(
            json.dumps(calibration_diagnostics) + "\n", encoding="utf-8"
        )
        diagnostics_receipt = {
            "schema": "pulp.gpu-dpr-cell-receipt.v1", "version": 1,
            "attempt_nonce": document["attempt_nonce"],
            "attempt_number": document["attempt_number"],
            "scenario_id": document["scenario"]["id"],
            "scenario_kind": document["scenario"]["kind"],
            "mode": document["mode"],
            "requested_dpr": document["requested_dpr"],
            "outcome": "inconclusive",
            "reason": "GPU timer did not detect the known-extra-work control",
            "dependencies": ["gpu:timer-calibration"],
            "producer_sha256": digest(producer),
            "diagnostics": calibration_diagnostics,
            "diagnostics_artifact": {
                "schema": "pulp.gpu-dpr-diagnostics-artifact.v1",
                "path": diagnostics_name,
                "sha256": digest(diagnostics_file),
            },
        }
        native_adapter.validate_measurement_receipt(
            document, diagnostics_receipt, tmp, producer
        )

        # A calibration dependency now carries the same mandatory evidence the
        # outer runner requires: an embedded diagnostics object, retained bytes
        # bound by path and digest, and the pinned producer identity.
        for label, drop in (
            ("diagnostics", "diagnostics"),
            ("artifact binding", "diagnostics_artifact"),
            ("producer digest", "producer_sha256"),
        ):
            planted = json.loads(json.dumps(diagnostics_receipt))
            del planted[drop]
            try:
                native_adapter.validate_measurement_receipt(
                    document, planted, tmp, producer
                )
            except ValueError:
                pass
            else:
                raise AssertionError(f"calibration receipt without {label} passed")
        planted = json.loads(json.dumps(diagnostics_receipt))
        planted["diagnostics_artifact"]["sha256"] = "0" * 64
        try:
            native_adapter.validate_measurement_receipt(document, planted, tmp, producer)
        except ValueError:
            pass
        else:
            raise AssertionError("calibration artifact digest drift passed")
        planted = json.loads(json.dumps(diagnostics_receipt))
        planted["diagnostics_artifact"]["path"] = "../escape.json"
        try:
            native_adapter.validate_measurement_receipt(document, planted, tmp, producer)
        except ValueError:
            pass
        else:
            raise AssertionError("calibration artifact path escape passed")
        def rebind(planted: dict) -> dict:
            """Keep a planted receipt's retained bytes consistent with its object."""
            payload = json.dumps(planted["diagnostics"]) + "\n"
            diagnostics_file.write_text(payload, encoding="utf-8")
            planted["diagnostics_artifact"]["sha256"] = hashlib.sha256(
                payload.encode("utf-8")
            ).hexdigest()
            return planted

        planted = rebind(json.loads(json.dumps(diagnostics_receipt)))
        planted["diagnostics"]["attempt_nonce"] = "0" * 32
        try:
            native_adapter.validate_measurement_receipt(document, planted, tmp, producer)
        except ValueError:
            pass
        else:
            raise AssertionError("unbound calibration diagnostics passed")
        for field, value in (
            ("failure_class", "unknown"),
            ("delta_ms", 0.0),
            ("control_detected", True),
        ):
            planted = json.loads(json.dumps(diagnostics_receipt))
            planted["diagnostics"][field] = value
            rebind(planted)
            try:
                native_adapter.validate_measurement_receipt(document, planted, tmp, producer)
            except ValueError:
                pass
            else:
                raise AssertionError(f"invalid diagnostics {field} passed")
        planted = json.loads(json.dumps(diagnostics_receipt))
        planted["diagnostics"]["trials"][0]["baseline"]["valid"] = False
        planted["diagnostics"]["trials"][0]["baseline"]["value_ms"] = 1.0
        rebind(planted)
        try:
            native_adapter.validate_measurement_receipt(document, planted, tmp, producer)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid calibration sample passed")

    print(
        "gpu_dpr_pulp_native_adapter_selftest=true real_capture_protocol=pass "
        "measured_producer_protocol=pass planted_digest_drift=pass "
        "planted_partial_scope=pass mandatory_calibration_evidence=pass"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
