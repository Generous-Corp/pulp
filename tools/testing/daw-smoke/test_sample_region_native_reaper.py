import importlib.util, pathlib, tempfile, unittest
p=pathlib.Path(__file__).with_name('sample_region_native_reaper.py'); s=importlib.util.spec_from_file_location('drv',p); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
class DriverShape(unittest.TestCase):
 def test_formats_are_explicit(self): self.assertEqual(m.FORMATS,('au','vst3','clap'))
 def test_missing_bundles_do_not_pass(self):
  with tempfile.TemporaryDirectory() as d: self.assertNotEqual(m.main(['--out',d]),0)
 def test_lua_is_checked_in(self): self.assertTrue(m.LUA.is_file())
if __name__=='__main__': unittest.main()
