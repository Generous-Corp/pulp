#!/usr/bin/env python3
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
class MiseProfileTests(unittest.TestCase):
 def test_profile_is_opt_in_and_locked(self):
  text=(ROOT/'.mise.toml').read_text()
  self.assertIn('auto_install = false',text); self.assertIn('locked = true',text)
  self.assertNotIn('auto_update',text); self.assertIn('install_visual_python_deps.sh',text)
 def test_no_lane_invokes_mise(self):
  for path in (ROOT/'.github',ROOT/'.shipyard',ROOT/'tools/ci'):
   if path.exists():
    for f in path.rglob('*'):
     if f.is_file() and f.suffix in {'.yml','.yaml','.toml','.sh','.py'}:
      text=f.read_text(errors='ignore')
      if 'mise run' in text: self.fail(f'lane depends on mise: {f}')
if __name__=='__main__': unittest.main()
