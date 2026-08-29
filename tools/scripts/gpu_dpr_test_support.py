#!/usr/bin/env python3
"""Private executable fixtures used by the A4 DPR runner self-test."""

from pathlib import Path

import gpu_first_visible_a3_acceptance as a3_acceptance
import test_gpu_first_visible_a3_acceptance as a3_fixture


def dependency_receipts(root: Path, *, ratified: bool = True) -> tuple[Path, str, Path]:
    """Build the real terminal A3/A2T fixture used by the checked-in validator."""
    budget_id = "pulp.editor-first-visible.v1"
    template = a3_fixture.make_fixture(root)
    a2t_path = root / "a2t.json"
    a2t_payload = a3_acceptance.load_json(a2t_path)
    a2t_payload["machine"] = {
        "architecture": "arm64",
        "chip": "Apple M3 Max",
        "gpu": "Apple M3 Max",
        "model_identifier": "Mac15,9",
        "model_name": "MacBook Pro",
        "os": "macOS 15.6",
    }
    a3_fixture.write_json(root, a2t_path.name, a2t_payload)
    binding_path = root / "binding.json"
    binding_payload = a3_acceptance.load_json(binding_path)
    binding_payload["a2t_receipt_sha256"] = a3_acceptance.sha256_bytes(
        a2t_path.read_bytes()
    )
    a3_fixture.write_json(root, binding_path.name, binding_payload)
    b4_path = root / "b4.json"
    b4_payload = a3_acceptance.load_json(b4_path)
    b4_payload["a2t_binding_sha256"] = a3_acceptance.sha256_bytes(
        binding_path.read_bytes()
    )
    a3_fixture.write_json(root, b4_path.name, b4_payload)
    receipt = a3_acceptance.materialize_auto_hashes(template, root)
    a2t = root / receipt["same_instance_a2t"]["a2t_receipt"]["path"]
    a3 = root / ("a3-receipt.json" if ratified else "a3-unratified.json")
    if not ratified:
        receipt["budget"]["status"] = "unratified"
    a3_fixture.write_json(root, a3.name, receipt)
    return a2t, budget_id, a3


def forged_minimal_dependencies(root: Path) -> tuple[Path, Path]:
    """Plant the legacy mini-shape that must never satisfy real A2T/A3 acceptance."""
    a2t = root / "forged-minimal-a2t.json"
    a3 = root / "forged-minimal-a3.json"
    a3_fixture.write_json(root, a2t.name, {
        "schema": "pulp.gpu-trace-overhead-acceptance.v1",
        "semantic_result": {
            "schema": "pulp.trace-gpu-analysis.v1", "question": "gpu-startup",
            "capture_complete": True, "verdict": "pass",
        },
        "acceptance": {"semantic_parity": "pass", "same_installed_prefix": "pass"},
    })
    a3_fixture.write_json(root, a3.name, {
        "schema": "dev.pulp.gpu-first-visible-a3-acceptance",
        "status": "complete",
        "budget": {"id": "pulp.editor-first-visible.v1", "status": "ratified"},
        "same_instance_a2t": {
            "status": "pass",
            "a2t_receipt": {"path": a2t.name, "sha256": a3_acceptance.sha256_bytes(a2t.read_bytes())},
        },
        "b4": {"disposition": "no-change", "status": "closed-no-change"},
    })
    return a2t, a3


def test_adapter_script(root: Path) -> Path:
    script = root / "skip-adapter.py"
    script.write_text(
        """#!/usr/bin/env python3
import argparse, json
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--receipt'); a=p.parse_args()
r=json.loads(Path(a.request).read_text())
Path(a.receipt).write_text(json.dumps({
  'schema':'pulp.gpu-dpr-cell-receipt.v1','version':1,
  'attempt_nonce':r['attempt_nonce'],
  'scenario_id':r['scenario']['id'],'scenario_kind':r['scenario']['kind'],
  'mode':r['mode'],'requested_dpr':r['requested_dpr'],'outcome':'skip',
  'reason':'real product adapter unavailable in selftest',
  'dependencies':['adapter:test-real-product']})+'\\n')
raise SystemExit(2)
""",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def timeout_adapter_script(root: Path) -> Path:
    script = root / "timeout-adapter.py"
    script.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n", encoding="utf-8")
    script.chmod(0o755)
    return script


def malformed_adapter_script(root: Path) -> Path:
    script = root / "malformed-adapter.py"
    script.write_text(
        """#!/usr/bin/env python3
import argparse
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--receipt'); a=p.parse_args()
Path(a.receipt).write_text('{not-json')
""",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def no_receipt_adapter_script(root: Path) -> Path:
    script = root / "no-receipt-adapter.py"
    script.write_text("#!/usr/bin/env python3\nraise SystemExit(0)\n", encoding="utf-8")
    script.chmod(0o755)
    return script


def wrong_nonce_adapter_script(root: Path) -> Path:
    script = root / "wrong-nonce-adapter.py"
    script.write_text(
        """#!/usr/bin/env python3
import argparse, json
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--request'); p.add_argument('--receipt'); a=p.parse_args()
r=json.loads(Path(a.request).read_text())
Path(a.receipt).write_text(json.dumps({
  'schema':'pulp.gpu-dpr-cell-receipt.v1','version':1,
  'attempt_nonce':'0'*32,
  'scenario_id':r['scenario']['id'],'scenario_kind':r['scenario']['kind'],
  'mode':r['mode'],'requested_dpr':r['requested_dpr'],'outcome':'skip',
  'reason':'wrong nonce control','dependencies':['nonce-control']})+'\\n')
raise SystemExit(2)
""",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def trace_analyzer_script(root: Path) -> Path:
    script = root / "trace-analyzer.py"
    script.write_text(
        """#!/usr/bin/env python3
import json, sys
question=sys.argv[2]
trace=sys.argv[sys.argv.index('--trace')+1]
data=open(trace,'rb').read()
prefix=b'real-trace-bytes:'
good=data.startswith(prefix) and len(data) == len(prefix)+32
verdict=('unverified' if question == 'gpu-startup' else 'pass') if good else 'unavailable'
evidence=[data[len(prefix):].decode()] if good else []
print(json.dumps({'schema':'pulp.trace-gpu-analysis.v1','question':question,
                  'verdict':verdict,'capture_complete':good,
                  'evidence_ids':evidence,
                  'category_scope':({'evidence_id':evidence[0],
                    'process_upid':7,'process_pid':42} if good else None),
                  'observed_categories':['gpu','js','layout','render','text'] if good else []}))
raise SystemExit(0 if verdict == 'pass' else 2)
""",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def noisy_adapter_script(root: Path) -> Path:
    script = root / "noisy-adapter.py"
    script.write_text(
        "#!/usr/bin/env python3\n"
        "import os, time\n"
        "os.write(1, b'x' * (1024 * 1024 + 65536))\n"
        "time.sleep(30)\n",
        encoding="utf-8",
    )
    script.chmod(0o755)
    return script


def exact_binary(root: Path) -> Path:
    binary = root / "forge-modular"
    binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    binary.chmod(0o755)
    return binary
