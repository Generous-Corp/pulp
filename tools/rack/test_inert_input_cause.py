#!/usr/bin/env python3
"""An inert input must say WHETHER THE DEFAULT is what makes it inert.

"input N changes no output when connected versus muted" is true and
unactionable. It names the symptom, and the commonest cause is not a miswired
jack: it is a modulated param whose DEFAULT sits in a flat region of its law,
where the CV is added to a knob position that cannot move. AnalogVcfT's minimoog
resonance is flat at 0.079 across knob 0.0-0.50, so a resonance CV on a knob
defaulted at 0.30 is arithmetically incapable of changing the output, and every
retry reproduces it exactly.

The gate's param sweep cannot catch that. It moves each knob across its FULL
range, where such a law does move, so the knob reads as live -- only the default
position is dead.

So the gate re-probes with the params pushed off their defaults, and says which
of the two it is. This test pins BOTH directions, because a hint that fired on
every inert input would tell a generation to raise a default on a jack that
genuinely does nothing -- confidently wrong, and worse than the terse message it
replaces.

The two fixtures differ by one expression.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent

# Resolved the way the generators resolve it, rather than hardcoded: this test
# is about the gate's message, and a wrong path would turn a real failure into a
# silent skip.
try:
    import sys
    sys.path.insert(0, str(HERE))
    import fetch_sdk  # noqa: E402
    SDK = pathlib.Path(fetch_sdk.installed_at() or "")
except Exception:  # pragma: no cover - the skip below reports it
    SDK = pathlib.Path("")

# One knob, one CV, one output. The CV's contribution is gated on the knob's
# position, which is the shape of a real calibration law's flat region: the jack
# is wired, and at the default it cannot move the output.
GATED_BY_DEFAULT = r'''
#include <rack.hpp>
#include <algorithm>
struct GatedFixture final : rack::engine::Module {
    enum { KNOB, PARAMS };
    enum { AUDIO_IN, CV_IN, INPUTS };
    enum { OUT, OUTPUTS };
    GatedFixture() {
        config(PARAMS, INPUTS, OUTPUTS, 0);
        // Default 0.30 sits below the knee, exactly like minimoog resonance.
        configParam(KNOB, 0.f, 1.f, 0.30f, "Knob");
        configInput(AUDIO_IN, "Audio");
        configInput(CV_IN, "CV");
        configOutput(OUT, "Out");
    }
    void process(const ProcessArgs& args) override {
        const float knob = params[KNOB].getValue();
        const float cv = inputs[CV_IN].getVoltage();
        // Flat below 0.5: the CV is READ and contributes nothing there.
        const float gain = knob < 0.5f ? 0.f : 1.f;
        const float in = inputs[AUDIO_IN].getVoltage();
        outputs[OUT].setVoltage(in * (1.f + gain * cv * 0.1f));
    }
};
rack::plugin::Model* modelGated =
    rack::createModel<GatedFixture, rack::app::ModuleWidget>("Gated");
'''

# Identical but for the CV term: read, then multiplied away. Dead at EVERY knob
# position, so moving the params cannot rescue it and the hint must stay silent.
DEAD_EVERYWHERE = GATED_BY_DEFAULT.replace(
    "const float gain = knob < 0.5f ? 0.f : 1.f;",
    "const float gain = 0.f;  // genuinely inert at every knob position",
).replace("GatedFixture", "DeadFixture").replace("modelGated", "modelDead") \
 .replace('"Gated"', '"Dead"')

HINT = "params moved off their defaults"


class InertInputCause(unittest.TestCase):
    def setUp(self) -> None:
        if not SDK or not (SDK / "libRack.dylib").is_file():
            raise unittest.SkipTest(f"Rack SDK unavailable at {SDK!s}")

    def run_gate(self, fixture_src: str, symbol: str) -> str:
        """Compile one fixture against the real gate and return its output."""
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = pathlib.Path(tmp)
            fixture = tmpdir / "fixture.cpp"
            shim = tmpdir / "shim.cpp"
            fixture.write_text(fixture_src)
            shim.write_text(
                "#include <rack.hpp>\n"
                f"extern rack::plugin::Model* {symbol};\n"
                "rack::engine::Module* forge_make_module() {"
                f" return {symbol}->createModule(); }}\n"
                'const char* forge_module_slug() { return "fixture"; }\n'
                "bool forge_module_is_generator() { return false; }\n"
                "bool forge_require_clock_contract() { return false; }\n"
                'const char* forge_output_name(int) { return "Out"; }\n'
                "int forge_clock_output() { return -1; }\n"
                "int forge_phase_output() { return -1; }\n"
                "int forge_rate_param() { return -1; }\n"
                "int forge_width_param() { return -1; }\n"
                "int forge_reset_input() { return -1; }\n"
                'const char* forge_input_role(int i) '
                '{ return i == 0 ? "Audio" : "Cv"; }\n'
                'const char* forge_output_role(int) { return "Audio"; }\n')
            binary = tmpdir / "gate"
            build = subprocess.run(
                ["clang++", "-std=c++20", "-O1", "-o", str(binary),
                 str(HERE / "behaviour_gate.cpp"), str(fixture), str(shim),
                 f"-I{SDK / 'include'}", f"-I{SDK / 'dep/include'}",
                 str(SDK / "libRack.dylib")],
                capture_output=True, text=True)
            if build.returncode != 0:
                self.fail("fixture did not build:\n" + build.stderr[-2000:])
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                                 env=dict(os.environ, DYLD_LIBRARY_PATH=str(SDK)))
            return run.stdout + run.stderr

    def test_a_default_that_hides_a_wired_jack_is_named(self) -> None:
        out = self.run_gate(GATED_BY_DEFAULT, "modelGated")
        self.assertIn("changes no output", out,
                      "the fixture is supposed to FAIL the inert-input check")
        self.assertIn(HINT, out,
                      "the jack is wired and only the default is at fault, so "
                      "the gate must say so:\n" + out[-1500:])

    def test_a_genuinely_dead_jack_gets_no_hint(self) -> None:
        # The control. Without it, an unconditional hint passes the test above
        # while telling a generation to fix a default on a jack that does
        # nothing at any default.
        out = self.run_gate(DEAD_EVERYWHERE, "modelDead")
        self.assertIn("changes no output", out,
                      "the fixture is supposed to FAIL the inert-input check")
        self.assertNotIn(HINT, out,
                         "moving the params cannot rescue a jack that is dead "
                         "at every position, so the hint must stay silent:\n"
                         + out[-1500:])


if __name__ == "__main__":
    unittest.main()
