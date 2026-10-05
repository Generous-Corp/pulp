import importlib.util, pathlib, tempfile, unittest
p=pathlib.Path(__file__).with_name('sample_region_native_reaper.py'); s=importlib.util.spec_from_file_location('drv',p); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
class DriverShape(unittest.TestCase):
 def test_formats_are_explicit(self): self.assertEqual(m.FORMATS,('au','vst3','clap'))
 def test_missing_bundles_do_not_pass(self):
  with tempfile.TemporaryDirectory() as d: self.assertNotEqual(m.main(['--out',d]),0)
 def test_lua_is_checked_in(self): self.assertTrue(m.LUA.is_file())
 def test_serialized_coefficient_guard_accepts_expected_value(self):
  with tempfile.NamedTemporaryFile('w', suffix='.rpp') as f:
   # PULP header at offset 0, version/count at offsets 4/8, id/value at 12.
   import base64, struct
   blob=b'PULP'+struct.pack('<IIIf', 1, 1, 2901, 0.5)
   f.write('<VST "x"\n"'+base64.b64encode(blob).decode()+'"\n>\n'); f.flush()
   self.assertEqual(m.serialized_coefficient_guard(f.name)[0], True)
 def test_serialized_coefficient_guard_rejects_stale_value(self):
  with tempfile.NamedTemporaryFile('w', suffix='.rpp') as f:
   import base64, struct
   blob=b'PULP'+struct.pack('<IIIf', 1, 1, 2901, -0.1570800096)
   f.write('<VST "x"\n"'+base64.b64encode(blob).decode()+'"\n>\n'); f.flush()
   ok, value, reason=m.serialized_coefficient_guard(f.name)
   self.assertFalse(ok); self.assertAlmostEqual(value, -0.1570800096, places=6); self.assertIn('expected', reason)
 def test_loaded_au_chunk_decodes_format_identity(self):
  chunk='<AU "AU: Sample Region Allpass (Pulp)" "Pulp: Sample Region Allpass" "" 1635083896 1399996784 1349872752\\n'
  identity=m.parse_loaded_fx_chunk('au',chunk)
  self.assertEqual(identity['type'],'aufx')
  self.assertEqual(identity['subtype'],'SrAp')
  self.assertEqual(identity['manufacturer'],'Pulp')
 def test_loaded_clap_chunk_decodes_bundle_identifier(self):
  chunk='<CLAP "CLAP: Sample Region Allpass (Pulp)" com.pulp.sample-region-allpass ""\\n'
  identity=m.parse_loaded_fx_chunk('clap',chunk)
  self.assertEqual(identity['ident'],m.EXPECTED_BUNDLE_ID)
 def test_loaded_vst3_chunk_decodes_module_name(self):
  chunk='<VST "VST3: Sample Region Allpass (Pulp) (mono)" "Sample Region Allpass.vst3" 0 "" 51558974{50554C5053414D50524547494F4E414C} ""\\n'
  identity=m.parse_loaded_fx_chunk('vst3',chunk)
  self.assertEqual(identity['module_name'],'Sample Region Allpass.vst3')
if __name__=='__main__': unittest.main()
