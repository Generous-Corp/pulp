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
import argparse, hashlib, json, struct
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--receipt'); a=p.parse_args()
r=json.loads(Path(a.request).read_text()); root=Path(a.receipt).parent
dpr=float(r['requested_dpr'])
if r['mode']=='configured_max': dpr=min(dpr, 2.0)
logical=r['scenario']['logical_size']; w=round(logical['width']*dpr); h=round(logical['height']*dpr)
capture=root/'measured-capture.png'
capture.write_bytes(b'\\x89PNG\\r\\n\\x1a\\n'+struct.pack('>I',13)+b'IHDR'+struct.pack('>II',w,h))
trace=root/'measured-trace.pftrace'; trace.write_bytes(b'real-perfetto-trace')
raw=root/'measured-raw.json'; raw.write_text(json.dumps({{'schema':'pulp.gpu-dpr-raw-samples.v1','version':1}})+'\\n')
inputs=root/'measured-input.json'; inputs.write_text(json.dumps({{'logical_input':True}})+'\\n')
def artifact(kind, file): return {{'kind':kind,'path':file.name,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()}}
receipt={{
 'schema':'pulp.gpu-dpr-cell-receipt.v1','version':1,
 'attempt_nonce':r['attempt_nonce'],'scenario_id':r['scenario']['id'],
 'scenario_kind':r['scenario']['kind'],'mode':r['mode'],'requested_dpr':r['requested_dpr'],
 'observed_dpr':dpr,'physical_size':{{'width':w,'height':h}},
 'content_digest':r['expected_content_digest'],'outcome':'pass','reason':None,'dependencies':[],
 'machine':{{'id':'selftest-m3','os':'macos','architecture':'arm64'}},
 'adapter':{{'class':'hardware','name':'Selftest GPU','backend':'Metal','driver':'selftest-1','authentic_identity':True}},
 'build_identity':{{'pulp_sha':r['pulp_sha']}},
 'measurement_scope':{{'schema':'pulp.gpu-dpr-native-measurement-scope.v1',
   'same_process':{{'adapter_identity':True,'capture':True,'frame_metrics':True,
     'memory_metrics':True,'logical_input':True,'trace_correlation':{scope_value}}},
   'audio_device_opened':False}},
 'artifacts':[artifact('capture',capture),artifact('trace',trace),
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
        "cell_key": "dense-text-thin-strokes__exact__dpr-1.5",
        "scenario": {
            "id": "dense-text-thin-strokes",
            "kind": "pulp_screenshot",
            "source": "dense-text-thin-strokes.ui.js",
            "logical_size": {"width": 640, "height": 360},
            "required_oracles": ["small_text", "thin_strokes", "logical_input"],
        },
        "mode": "exact",
        "requested_dpr": 1.5,
        "expected_content_digest": expected_digest,
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

    print(
        "gpu_dpr_pulp_native_adapter_selftest=true real_capture_protocol=pass "
        "measured_producer_protocol=pass planted_digest_drift=pass "
        "planted_partial_scope=pass"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
