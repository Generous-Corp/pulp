import importlib.util, pathlib, tempfile, unittest
p=pathlib.Path(__file__).with_name('sample_region_native_reaper.py'); s=importlib.util.spec_from_file_location('drv',p); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
class DriverShape(unittest.TestCase):
 def test_formats_are_explicit(self): self.assertEqual(m.FORMATS,('au','vst3','clap'))
 def test_missing_bundles_do_not_pass(self):
  with tempfile.TemporaryDirectory() as d: self.assertNotEqual(m.main(['--out',d]),0)
 def test_lua_is_checked_in(self): self.assertTrue(m.LUA.is_file())
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
