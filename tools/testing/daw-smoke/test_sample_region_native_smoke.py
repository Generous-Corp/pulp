#!/usr/bin/env python3
"""Negative-only parser tests; successful PUB-04 verdicts require real REAPER receipts."""
from __future__ import annotations
import importlib.util, json, pathlib, unittest
HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('sample_region_native_smoke',HERE/'sample_region_native_smoke.py'); assert spec and spec.loader
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
def receipt(**overrides):
 v={'packet':'PKT-F4-01','format':'vst3','host':'REAPER','host_version':'7.78','plugin_path':'/tmp/Sample Region Allpass.vst3','bundle_id':'com.pulp.sample-region-allpass','host_instance':'real-reaper','installed':True,'signed':True,'automation':True,'state_save':True,'state_reload':True,'audio':True,'reload':True,'zero_pdc':True,'parameter_identity':True,'host_parameter_ids':['index:0;ident:;name:Coefficient'],'parameter_ids':['index:0;ident:;name:Coefficient'],'parameter_order':['index:0;ident:;name:Coefficient'],'pdc_samples':0,'audio_peak':0.25,'saved_generation':4,'reload_generation':4,'state_before_sha256':'a','state_after_sha256':'a','state_hash_equal':True,'wav_exists':True,'wav_sha256':'b','audio_oracle_pass':True,'automation_points':[0.25,0.75],'pdc_api':'TrackFX_GetNamedConfigParm:pdc'}; v.update(overrides); return v
class NativeProofNegative(unittest.TestCase):
 def test_scan_only_is_inconclusive(self): self.assertEqual(mod.analyze_output('plugin scanned\n',expected_format='vst3').code,mod.EXIT_INCONCLUSIVE)
 def test_wrong_parameter_identity_fails(self): self.assertEqual(mod.validate_receipt(receipt(host_parameter_ids=['index:1'],parameter_ids=['index:0;ident:;name:Coefficient'])).code,mod.EXIT_FAIL)
 def test_nonzero_pdc_fails(self): self.assertEqual(mod.validate_receipt(receipt(pdc_samples=1)).code,mod.EXIT_FAIL)
 def test_missing_real_audio_oracle_is_inconclusive(self): self.assertEqual(mod.validate_receipt(receipt(audio_oracle_pass=False)).code,mod.EXIT_INCONCLUSIVE)
 def test_missing_state_hash_is_inconclusive(self): self.assertEqual(mod.validate_receipt(receipt(state_hash_equal=False)).code,mod.EXIT_INCONCLUSIVE)
 def test_missing_automation_observation_is_inconclusive(self): self.assertEqual(mod.validate_receipt(receipt(automation_points=[])).code,mod.EXIT_INCONCLUSIVE)
if __name__=='__main__': unittest.main()
