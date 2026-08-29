#!/usr/bin/env python3
"""Protocol and planted-negative tests for the A4 browser adapter."""

from __future__ import annotations

import hashlib
import json
import stat
import struct
import tempfile
from pathlib import Path

import gpu_dpr_evidence as evidence
import gpu_dpr_web_adapter as web


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value) + "\n", encoding="utf-8")


def request(root: Path) -> dict:
    source = root / "source.mjs"
    return {
        "schema": web.REQUEST_SCHEMA, "version": 1,
        "attempt_nonce": "a" * 32, "attempt_number": 1,
        "cell_key": "super-convolver-web__exact__dpr-1",
        "scenario": {"id": "super-convolver-web", "kind": "maintained_web_canary",
                     "source": "source.mjs", "logical_size": {"width": 760, "height": 520}},
        "mode": "exact", "requested_dpr": 1,
        "expected_content_digest": hashlib.sha256(source.read_bytes()).hexdigest(),
        "trial_contract": {"fresh_process_first_frame_trials": 20},
        "pulp_sha": "1" * 40, "pulp_source_root": str(root),
    }


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pulp-dpr-web-") as temporary:
        root = Path(temporary); (root / "source.mjs").write_text("export default true\n")
        req = request(root); cell = root / "cell"; cell.mkdir()
        browser = root / "fake-browser"
        browser.write_text("#!/bin/sh\necho definitely-not-chrome\n"); browser.chmod(0o755)
        try: web.browser_product_identity(browser)
        except ValueError: pass
        else: raise AssertionError("fake browser product identity passed")

        native = json.loads(json.dumps(req)); native["scenario"]["id"] = "dense-text-thin-strokes"
        native["scenario"]["kind"] = "pulp_screenshot"
        request_path = root / "request.json"; receipt_path = root / "receipt.json"
        write_json(request_path, native)
        assert web.run(request_path, receipt_path) == 3
        assert "owns only" in json.loads(receipt_path.read_text())["reason"]

        capture = cell / "capture.png"
        capture.write_bytes(b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">II", 760, 520))
        trace = cell / "trace.json"; trace.write_text('{"traceEvents":[]}')
        inputs = cell / "input.json"; write_json(inputs, {"ok": True})
        browser_digest = hashlib.sha256(browser.read_bytes()).hexdigest()
        adapter = {"class":"hardware","api":"webgl2","name":"ANGLE Metal",
                   "backend":"WebGL2","driver":"Apple GPU | WebGL 2.0",
                   "authentic_identity":True}
        first = [10 + i / 10 for i in range(20)]
        trials = [{"schema":web.FIRST_FRAME_SCHEMA,"version":1,
                   "attempt_nonce":req["attempt_nonce"],"attempt_number":1,"pid":1000+i,
                   "producer_sha256":browser_digest,"content_digest":req["expected_content_digest"],
                   "pulp_sha":req["pulp_sha"],"first_frame_time_ms":first[i],"adapter":adapter}
                  for i in range(20)]
        raw = cell / "raw.json"
        write_json(raw, {"metrics":{"gpu_frame_time":[0.2] * 30},
                         "fresh_process_trials":trials})
        artifact = lambda kind, path: {"kind":kind,"path":path.name,
            "sha256":hashlib.sha256(path.read_bytes()).hexdigest()}
        receipt = {"schema":web.RECEIPT_SCHEMA,"version":1,
            "attempt_nonce":req["attempt_nonce"],"attempt_number":1,
            "scenario_id":"super-convolver-web","scenario_kind":"maintained_web_canary",
            "mode":"exact","requested_dpr":1,"observed_dpr":1,
            "physical_size":{"width":760,"height":520},
            "content_digest":req["expected_content_digest"],"outcome":"pass",
            "adapter":adapter,"build_identity":{"pulp_sha":req["pulp_sha"]},
            "measurement_scope":{"schema":web.SCOPE_SCHEMA,
              "same_process":{key:True for key in web.SAME_PROCESS_FIELDS},
              "audio_device_opened":False},
            "artifacts":[artifact("capture",capture),artifact("trace",trace),
                         artifact("raw_samples",raw),artifact("input_receipt",inputs)]}
        assert web.validate_receipt(req, receipt, cell, browser, browser)["outcome"] == "pass"
        planted = json.loads(raw.read_text()); planted["metrics"]["gpu_frame_time"][0] = 0
        write_json(raw, planted); receipt["artifacts"][2] = artifact("raw_samples", raw)
        try: web.validate_receipt(req, receipt, cell, browser, browser)
        except ValueError: pass
        else: raise AssertionError("zero browser GPU timing passed")
        planted["metrics"]["gpu_frame_time"][0] = 0.2
        planted["fresh_process_trials"][1]["pid"] = planted["fresh_process_trials"][0]["pid"]
        write_json(raw, planted); receipt["artifacts"][2] = artifact("raw_samples", raw)
        try: web.validate_receipt(req, receipt, cell, browser, browser)
        except ValueError: pass
        else: raise AssertionError("reused browser process passed")
        receipt["adapter"] = dict(adapter, name="SwiftShader Device")
        try: web.validate_receipt(req, receipt, cell, browser, browser)
        except ValueError: pass
        else: raise AssertionError("software WebGL adapter passed")

        manifest = {"trial_contract":{"required_trace_categories":["render","gpu","text","js","layout"]}}
        trace_raw = {"trace":{"complete":True,"kind":"browser-devtools","process_pid":77}}
        events = [{"name":f"pulp.dpr.{req['attempt_nonce']}.{category}","ph":"X","pid":77,"dur":1}
                  for category in manifest["trial_contract"]["required_trace_categories"]]
        write_json(trace, {"traceEvents":events})
        assert evidence.validate_trace(trace_raw, manifest, trace, {}, req["attempt_nonce"],
                                       "maintained_web_canary") == [req["attempt_nonce"]]
        mixed = json.loads(trace.read_text()); mixed["traceEvents"][1]["pid"] = 88; write_json(trace,mixed)
        try: evidence.validate_trace(trace_raw,manifest,trace,{},req["attempt_nonce"],"maintained_web_canary")
        except evidence.EvidenceError: pass
        else: raise AssertionError("mixed renderer trace passed")
        foreign = {"traceEvents":events + [{"name":f"pulp.dpr.{'b'*32}.gpu","ph":"X","pid":77,"dur":1}]}
        write_json(trace,foreign)
        try: evidence.validate_trace(trace_raw,manifest,trace,{},req["attempt_nonce"],"maintained_web_canary")
        except evidence.EvidenceError: pass
        else: raise AssertionError("foreign nonce trace passed")

        scenario = dict(req["scenario"], required_oracles=["authentic_webgl"])
        plan = {"pulp_sha": req["pulp_sha"], "forge_sha": None}
        unattested = {
            "machine": {"id":"test","os":"test","architecture":"test"},
            "adapter": adapter,
            "build_identity": {"pulp_sha": req["pulp_sha"]},
        }
        try: evidence.validate_identity(unattested, scenario, plan)
        except evidence.EvidenceError: pass
        else: raise AssertionError("unattested browser evidence passed")
    print("gpu_dpr_web_adapter_selftest=true planted_fake_browser=pass planted_native=pass planted_zero_gpu=pass planted_reused_process=pass planted_trace_scope=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
