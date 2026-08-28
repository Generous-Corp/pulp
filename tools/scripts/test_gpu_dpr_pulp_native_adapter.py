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


def request(root: Path, expected_digest: str) -> dict:
    return {
        "schema": "pulp.gpu-dpr-cell-request.v1",
        "version": 1,
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
        env["PULP_DPR_SOURCE_ROOT"] = str(ROOT)
        env["PULP_DPR_SCREENSHOT_BIN"] = str(executable)
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 3, completed
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert receipt["outcome"] == "inconclusive"
        assert "a2t:correlated-cell-trace" in receipt["dependencies"]
        assert "small-text-legibility-oracle:dense-text-thin-strokes" in receipt["dependencies"]
        preflight = json.loads((tmp / "preflight.json").read_text(encoding="utf-8"))
        assert preflight["physical_size"] == {"width": 960, "height": 540}
        assert preflight["source_sha256"] == digest(source)
        assert preflight["capture_backend_requested"] == "skia"
        assert "gpu_frame_time" in preflight["not_claimed"]

        # Negative control: a changed source digest is rejected before capture.
        document["expected_content_digest"] = "0" * 64
        write_json(request_path, document)
        (tmp / "capture.png").unlink()
        completed = subprocess.run(
            [sys.executable, str(ADAPTER), "--request", str(request_path),
             "--receipt", str(receipt_path)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert completed.returncode == 3
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        assert "source digest differs" in receipt["reason"]
        assert "native-capture:dense-text-thin-strokes" in receipt["dependencies"]
        assert not (tmp / "capture.png").exists()

    print("gpu_dpr_pulp_native_adapter_selftest=true real_capture_protocol=pass planted_digest_drift=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
