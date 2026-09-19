#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("sample_region_native_smoke", HERE / "sample_region_native_smoke.py")
assert spec and spec.loader
mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)


def receipt(**overrides):
    value = {
        "packet": "PKT-F4-01", "format": "vst3", "host": "REAPER",
        "host_version": "7.78", "plugin_path": "/tmp/Pulp Sample Region.vst3",
        "bundle_id": "com.pulp.sample-region-allpass", "host_instance": "reaper-abc",
        "installed": True, "signed": True, "automation": True,
        "state_save": True, "state_reload": True, "audio": True,
        "reload": True, "zero_pdc": True, "parameter_identity": True,
        "host_parameter_ids": ["index:0;name:Coefficient"], "parameter_order": ["index:0;name:Coefficient"], "parameter_ids": ["index:0;name:Coefficient"],
        "pdc_samples": 0, "audio_peak": 0.25,
        "saved_generation": 4, "reload_generation": 4, "state_before_sha256": "a", "state_after_sha256": "a", "state_hash_equal": True, "wav_exists": True, "wav_sha256": "b", "audio_oracle_pass": True, "automation_points": [0.25, 0.75], "pdc_api": "TrackFX_GetNamedConfigParm:pdc",
    }
    value.update(overrides)
    return value


class NativeProof(unittest.TestCase):
    def test_complete_receipt_passes(self):
        value = mod.validate_receipt(receipt())
        self.assertEqual(value.code, mod.EXIT_PASS)

    def test_each_format_is_checked(self):
        for fmt in mod.FORMATS:
            value = mod.validate_receipt(receipt(format=fmt), expected_format=fmt)
            self.assertEqual(value.code, mod.EXIT_PASS, fmt)

    def test_missing_host_observation_is_not_a_pass(self):
        value = mod.validate_receipt(receipt(audio=False))
        self.assertEqual(value.code, mod.EXIT_INCONCLUSIVE)

    def test_scan_only_output_is_not_a_pass(self):
        value = mod.analyze_output("plugin scanned\n", expected_format="vst3")
        self.assertEqual(value.code, mod.EXIT_INCONCLUSIVE)

    def test_wrong_parameter_order_fails(self):
        value = mod.validate_receipt(receipt(host_parameter_ids=["index:1"], parameter_ids=["index:1"], parameter_order=["index:0;name:Coefficient"]))
        self.assertEqual(value.code, mod.EXIT_FAIL)

    def test_nonzero_pdc_fails(self):
        value = mod.validate_receipt(receipt(pdc_samples=1))
        self.assertEqual(value.code, mod.EXIT_FAIL)

    def test_json_marker_is_extracted(self):
        line = "[sample-region-f4] " + json.dumps(receipt())
        value = mod.analyze_output("REAPER\n" + line + "\n", expected_format="vst3")
        self.assertEqual(value.code, mod.EXIT_PASS)


if __name__ == "__main__":
    unittest.main()
