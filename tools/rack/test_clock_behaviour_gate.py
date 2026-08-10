#!/usr/bin/env python3
"""Real-Rack negative controls for the prompt-derived clock contract."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SDK = pathlib.Path.home() / "Library/Application Support/Forge Modular/sdk/Rack-SDK"

FIXTURE = r'''
#include <rack.hpp>
#include <algorithm>
#include <cmath>

struct ClockFixture final : rack::engine::Module {
    enum { RATE, WIDTH, PARAMS };
    enum { RATE_CV, RESET, SYNC, INPUTS };
    enum { CLOCK, PHASE, OUTPUTS };
    double phase = 0.0;
    bool reset_high = false;
    bool sync_high = false;

    ClockFixture() {
        config(PARAMS, INPUTS, OUTPUTS, 0);
        configParam(RATE, -3.f, 3.f, 0.f, "Rate");
        configParam(WIDTH, .05f, .95f, .5f, "Width");
        configInput(RATE_CV, "Rate CV");
        configInput(RESET, "Reset");
        configInput(SYNC, "Clock sync");
        configOutput(CLOCK, "Clock");
        configOutput(PHASE, "Phase");
    }

    void process(const ProcessArgs& args) override {
        const bool high = inputs[RESET].getVoltage() >= 2.f;
#ifndef IGNORE_RESET
        if (high && !reset_high) phase = 0.0;
#endif
        reset_high = high;
        const bool sync = inputs[SYNC].getVoltage() >= 2.f;
        if (sync && !sync_high) phase = 0.0;
        sync_high = sync;
#ifndef IGNORE_RATE
        float rate = params[RATE].getValue();
#else
        float rate = 0.f;
#endif
#ifndef IGNORE_RATE_CV
        rate += inputs[RATE_CV].getVoltage();
#endif
        const double hz = std::exp2(rate);
        phase += hz * args.sampleTime;
        phase -= std::floor(phase);
#ifndef IGNORE_WIDTH
        const float width = params[WIDTH].getValue();
#else
        const float width = .5f;
#endif
        outputs[CLOCK].setVoltage(phase < width ? 10.f : 0.f);
#ifndef FREEZE_PHASE
        outputs[PHASE].setVoltage(float(phase * 10.0));
#else
        outputs[PHASE].setVoltage(0.f);
#endif
    }
};

rack::engine::Module* forge_make_module() { return new ClockFixture; }
const char* forge_module_slug() { return "CLOCKFIXTURE"; }
bool forge_module_is_generator() { return true; }
const char* forge_input_role(int i) {
    return i == 0 ? "Pitch" : (i == 1 ? "Trigger" : "Clock");
}
const char* forge_output_role(int i) { return i == 0 ? "Clock" : "Cv"; }
const char* forge_output_name(int i) { return i == 0 ? "Clock" : "Phase"; }
bool forge_require_clock_contract() { return true; }
int forge_clock_output() { return 0; }
int forge_phase_output() { return 1; }
int forge_rate_param() { return 0; }
int forge_width_param() { return 1; }
int forge_reset_input() { return 1; }
'''


class ClockBehaviourGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not (SDK / "libRack.dylib").is_file():
            raise unittest.SkipTest(f"Rack SDK unavailable at {SDK}")

    def run_fixture(self, define: str | None = None) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as root:
            root = pathlib.Path(root)
            fixture = root / "fixture.cpp"
            fixture.write_text(FIXTURE)
            executable = root / "gate"
            command = [
                "clang++", "-std=c++20", "-O1", "-o", str(executable),
                str(HERE / "behaviour_gate.cpp"), str(fixture),
                f"-I{SDK / 'include'}", f"-I{SDK / 'dep/include'}",
                str(SDK / "libRack.dylib"),
            ]
            if define:
                command.append(f"-D{define}")
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stderr)
            return subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=30,
                env=dict(os.environ, DYLD_LIBRARY_PATH=str(SDK)))

    def test_complete_clock_passes(self) -> None:
        result = self.run_fixture()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_frozen_width_fails(self) -> None:
        result = self.run_fixture("IGNORE_WIDTH")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("WIDTH must materially increase", result.stdout)

    def test_ignored_reset_fails(self) -> None:
        result = self.run_fixture("IGNORE_RESET")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("RESET must return PHASE", result.stdout)

    def test_frozen_phase_fails(self) -> None:
        result = self.run_fixture("FREEZE_PHASE")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("requested PHASE must move", result.stdout)

    def test_non_monotonic_rate_fails(self) -> None:
        result = self.run_fixture("IGNORE_RATE")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("RATE must monotonically increase", result.stdout)

    def test_ignored_rate_cv_fails(self) -> None:
        result = self.run_fixture("IGNORE_RATE_CV")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("input 0 changes no output", result.stdout)


if __name__ == "__main__":
    unittest.main()
