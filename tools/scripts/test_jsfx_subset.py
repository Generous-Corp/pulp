import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parent / "jsfx_subset.py"
REPO_ROOT = SCRIPT.parents[2]


class JsfxSubsetScriptTests(unittest.TestCase):
    def run_script(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            check=False,
            capture_output=True,
            text=True,
        )

    def write_temp_jsfx(self, content: str) -> Path:
        tmp = tempfile.NamedTemporaryFile("w", suffix=".jsfx", delete=False)
        tmp.write(textwrap.dedent(content))
        tmp.flush()
        tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink(missing_ok=True))
        return Path(tmp.name)

    def test_doctor_accepts_shipped_gain_example(self) -> None:
        path = REPO_ROOT / "examples/jsfx-gain/PulpJsfxGain.jsfx"
        result = self.run_script("doctor", "--file", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("supported: yes", result.stdout)
        self.assertIn("sections: init, slider, sample", result.stdout)

    def test_doctor_rejects_gfx_section(self) -> None:
        path = self.write_temp_jsfx(
            """
            desc:Bad Example
            slider1:1<0,2,0.01>Gain
            @init
              gain = 1;
            @gfx
              gfx_clear = 0;
            """
        )
        result = self.run_script("doctor", "--file", str(path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("unsupported JSFX sections", result.stderr)
        self.assertIn("@gfx", result.stderr)

    def test_doctor_rejects_gfx_section_with_size_directive(self) -> None:
        # `@gfx WIDTH HEIGHT` is the canonical REAPER form documenting the
        # preferred plugin-window size. Pre-fix the regex was anchored to
        # end-of-line and silently skipped this entire shape, letting `@gfx
        # 200 200` slip past the bounded-subset check. The fix captures only
        # the directive name and ignores trailing tokens so the boundary
        # actually holds for the form developers will paste in.
        path = self.write_temp_jsfx(
            """
            desc:Bad Example With Size
            slider1:1<0,2,0.01>Gain
            @init
              gain = 1;
            @gfx 200 200
              gfx_clear = 0;
            """
        )
        result = self.run_script("doctor", "--file", str(path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("unsupported JSFX sections", result.stderr)
        self.assertIn("@gfx", result.stderr)

    def test_doctor_emits_json_summary(self) -> None:
        path = self.write_temp_jsfx(
            """
            desc:JSON Example
            slider1:1<0,2,0.01>Gain
            slider2:0.5<0,1,0.01>Depth
            @init
              gain = 1;
            @slider
              gain = slider1;
            @sample
              spl0 *= gain;
              spl1 *= gain;
            """
        )
        result = self.run_script("doctor", "--file", str(path), "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["desc"], "JSON Example")
        self.assertEqual(payload["slider_count"], 2)
        self.assertEqual(payload["sections"], ["init", "slider", "sample"])
        self.assertEqual(payload["unsupported_sections"], [])
        self.assertTrue(payload["supported"])

    def test_doctor_rejects_duplicate_slider(self) -> None:
        path = self.write_temp_jsfx(
            """
            desc:Duplicate Slider Example
            slider1:1<0,2,0.01>Gain
            slider1:0.5<0,1,0.01>Depth
            @sample
              spl0 *= slider1;
              spl1 *= slider1;
            """
        )
        result = self.run_script("doctor", "--file", str(path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate slider1", result.stderr)

    def test_doctor_rejects_out_of_range_slider(self) -> None:
        path = self.write_temp_jsfx(
            """
            desc:Out Of Range Slider Example
            slider65:1<0,2,0.01>Gain
            @sample
              spl0 *= slider65;
              spl1 *= slider65;
            """
        )
        result = self.run_script("doctor", "--file", str(path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("slider index out of range", result.stderr)
        self.assertIn("slider65", result.stderr)

    def test_doctor_rejects_duplicate_section(self) -> None:
        path = self.write_temp_jsfx(
            """
            desc:Duplicate Section Example
            @sample
              spl0 *= 0.5;
            @sample
              spl1 *= 0.5;
            """
        )
        result = self.run_script("doctor", "--file", str(path))
        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate @sample section", result.stderr)


if __name__ == "__main__":
    unittest.main()
