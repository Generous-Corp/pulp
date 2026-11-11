#!/usr/bin/env python3
"""Bounded real-fixture controls; reuse the production receipt validator."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / "tools"))
import graph_performance_qualification as qualification

out = Path(__file__).resolve().parent
receipt = root / "build/g12-review-final-pilot/receipt.json"
binary = root / "build/test/pulp-test-group-trigger-graph"
qualification.validate_receipt(receipt, root)
qualification.run_controls(receipt, root)
results = []
for backend in ("direct", "routed"):
    for fault in ("", "omit_commit", "wrong_gain", ""):
        label = backend + "-" + (fault or ("restored" if results and results[-1]["fault"] else "positive"))
        env = dict(os.environ, G12_ACTIVATION_RATE="48000", G12_ACTIVATION_FRAMES="128",
                   G12_ACTIVATION_CALLBACKS="64", G12_ACTIVATION_BACKEND=backend,
                   G12_ACTIVATION_FAULT=fault)
        command = [str(binary), "G12 paced activation receipt cell"]
        raw = out / (label + ".log")
        with raw.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, env=env, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=30)
        rejection = None
        try:
            qualification.parse_activation_output(raw.read_text(encoding="utf-8"))
        except qualification.ReceiptError as exc:
            rejection = str(exc)
        assert (result.returncode != 0 and rejection) if fault else (result.returncode == 0 and rejection is None)
        results.append({"backend": backend, "fault": fault, "label": label,
                        "command": command, "returncode": result.returncode,
                        "independent_validator_rejection": rejection,
                        "raw_sha256": hashlib.sha256(raw.read_bytes()).hexdigest()})
(out / "real-fixture-controls.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
qualification.validate_receipt(receipt, root)
print("real_fixture_controls=pass positive=2 broken=4 restored=2 receipt_restored=pass")
