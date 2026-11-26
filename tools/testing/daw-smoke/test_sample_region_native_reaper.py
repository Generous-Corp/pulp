import contextlib, importlib.util, pathlib, tempfile, unittest
p=pathlib.Path(__file__).with_name('sample_region_native_reaper.py'); s=importlib.util.spec_from_file_location('drv',p); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
@contextlib.contextmanager
def rpp_file(text):
 # A closed file in a temp directory: Windows cannot reopen a NamedTemporaryFile
 # that is still open, which is what the guard under test does.
 with tempfile.TemporaryDirectory() as d:
  path=pathlib.Path(d)/'project.rpp'
  path.write_text(text, encoding='utf-8')
  yield str(path)
class DriverShape(unittest.TestCase):
 def test_formats_are_explicit(self): self.assertEqual(m.FORMATS,('au','vst3','clap'))
 def test_overall_status_preserves_pre_render_failure(self):
  self.assertEqual(m.overall_status([{'status':'failed'}, {'status':'passed'}]), 'failed')
 def test_overall_status_keeps_unknown_results_inconclusive(self):
  self.assertEqual(m.overall_status([{'status':'inconclusive'}]), 'inconclusive')
 def test_missing_bundles_do_not_pass(self):
  with tempfile.TemporaryDirectory() as d: self.assertNotEqual(m.main(['--out',d]),0)
 def test_lua_is_checked_in(self): self.assertTrue(m.LUA.is_file())
 def test_serialized_coefficient_guard_accepts_expected_value(self):
  # PULP header at offset 0, version/count at offsets 4/8, id/value at 12.
  import base64, struct
  blob=b'PULP'+struct.pack('<IIIf', 1, 1, 2901, 0.5)
  with rpp_file('<VST "x"\n"'+base64.b64encode(blob).decode()+'"\n>\n') as name:
   self.assertEqual(m.serialized_coefficient_guard(name)[0], True)
 def test_serialized_coefficient_guard_rejects_stale_value(self):
  import base64, struct
  blob=b'PULP'+struct.pack('<IIIf', 1, 1, 2901, -0.1570800096)
  with rpp_file('<VST "x"\n"'+base64.b64encode(blob).decode()+'"\n>\n') as name:
   ok, value, reason=m.serialized_coefficient_guard(name)
   self.assertFalse(ok); self.assertAlmostEqual(value, -0.1570800096, places=6); self.assertIn('expected', reason)
 def test_serialized_coefficient_guard_decodes_au_plist_state(self):
  import base64, struct
  blob=b'PULP'+struct.pack('<IIIf', 1, 1, 2901, 0.5)
  plist='<?xml version="1.0"?><plist><dict><key>pulp-state</key><data>'+base64.b64encode(blob).decode()+'</data></dict></plist>'
  outer=base64.b64encode(plist.encode()).decode()
  with rpp_file('<AU "x" "y" "" 1 2 3\n  '+outer+'\n>\n') as name:
   self.assertEqual(m.serialized_coefficient_guard(name)[0:2], (True, 0.5))
 def test_serialized_coefficient_guard_decodes_clap_nested_state(self):
  import base64, struct
  blob=b'PULP'+struct.pack('<IIIf', 1, 1, 2901, 0.5)
  state=base64.b64encode(blob).decode()
  with rpp_file('<CLAP "x" com.pulp.sample-region-allpass ""\n<IN_PINS\n>\n<STATE\n  '+state+'\n>\n>\n') as name:
   self.assertEqual(m.serialized_coefficient_guard(name)[0:2], (True, 0.5))
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
class IsolatedReaperProfile(unittest.TestCase):
 """The lane must never scan the user's plug-in folders."""
 def test_every_launch_reads_an_isolated_profile(self):
  import subprocess, sys
  from unittest import mock
  with tempfile.TemporaryDirectory() as d:
   root=pathlib.Path(d); bundle=root/'Thing.vst3'; (bundle/'Contents'/'MacOS').mkdir(parents=True)
   exe=bundle/'Contents'/'MacOS'/'Thing'; exe.write_bytes(b'x'); exe.chmod(0o755)
   seen=[]
   def fake_run(cmd, **kw):
    seen.append(list(cmd))
    ini=pathlib.Path(cmd[cmd.index('-cfgfile')+1]); text=ini.read_text(encoding='utf-8')
    roots=[l.split('=',1)[1] for l in text.splitlines() if m.guard.SCAN_PATH_KEY.match(l)]
    seen.append(roots)
    scan=ini.parent/'scan'
    seen.append(sorted(x.name for x in scan.iterdir()))
    return subprocess.CompletedProcess(cmd,0,stdout='',stderr='')
   fake=root/'REAPER'; fake.write_bytes(b'x'); fake.chmod(0o755)
   with mock.patch.object(m, 'bundle_executable', return_value=exe), \
        mock.patch.object(m.guard, 'running_reaper_pids', return_value=[]), \
        mock.patch.object(m.guard, 'scoped_persistence_opt_out', contextlib.nullcontext), \
        mock.patch.object(m.subprocess, 'run', side_effect=fake_run), \
        mock.patch.dict('os.environ', {'REAPER_BIN': str(fake)}):
    m.run('vst3', bundle, root/'out', 5)
   cmd, roots, listing = seen[0], seen[1], seen[2]
   self.assertIn('-cfgfile', cmd)
   self.assertTrue(roots)
   scan_root=str(pathlib.Path(cmd[cmd.index('-cfgfile')+1]).parent/'scan')
   self.assertEqual(set(roots), {scan_root})
   self.assertEqual(listing, ['Thing.vst3'])
 def test_a_running_reaper_is_not_launched_over(self):
  from unittest import mock
  with mock.patch.object(m.guard, 'running_reaper_pids', return_value=[99]), \
       mock.patch.object(m.subprocess, 'run') as run:
   rec=m.run('vst3', pathlib.Path('/nonexistent/Thing.vst3'), pathlib.Path('/nonexistent/out'), 5)
  self.assertEqual(rec['status'],'inconclusive'); self.assertIn('another REAPER', rec['reason']); run.assert_not_called()

if __name__=='__main__': unittest.main()
