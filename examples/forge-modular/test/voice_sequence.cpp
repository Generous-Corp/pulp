// Drive the real Rack module, not a second copy of its cursor arithmetic.
// SEQModule is intentionally private to voice.cpp, so including the production
// translation unit is what lets this test exercise that exact implementation.
#include "../src/voice.cpp"

#include <cmath>
#include <cstdio>

rack::plugin::Plugin* pluginInstance = nullptr;

namespace {

using L = forge_modular::SEQLayout;

bool expect_voltage(SEQModule& seq, float expected, const char* moment) {
    const float actual = seq.outputs[L::CV_OUTPUT].getVoltage();
    if (std::fabs(actual - expected) < 1e-6f) return true;
    std::fprintf(stderr, "%s: expected %.1f V, got %.6f V\n",
                 moment, expected, actual);
    return false;
}

void process_at(SEQModule& seq, const rack::engine::Module::ProcessArgs& args,
                float clock_voltage, float reset_voltage = 0.0f) {
    seq.inputs[L::CLOCK_INPUT].setVoltage(clock_voltage);
    seq.inputs[L::RESET_INPUT].setVoltage(reset_voltage);
    seq.process(args);
}

bool clock(SEQModule& seq, const rack::engine::Module::ProcessArgs& args,
           float expected, const char* moment) {
    process_at(seq, args, 0.0f);
    process_at(seq, args, 10.0f);
    return expect_voltage(seq, expected, moment);
}

}  // namespace

int main() {
    SEQModule seq;
    rack::engine::Module::ProcessArgs args{};
    args.sampleRate = 48000.0f;
    args.sampleTime = 1.0f / args.sampleRate;

    for (int i = 0; i < SEQModule::kSteps; ++i)
        seq.params[L::STEP1_PARAM + i].setValue(static_cast<float>(i + 1));

    process_at(seq, args, 0.0f);
    if (!expect_voltage(seq, 1.0f, "idle after construction")) return 1;
    if (seq.outputs[L::GATE_OUTPUT].getVoltage() != 0.0f) {
        std::fprintf(stderr, "idle after construction: gate was high\n");
        return 2;
    }

    process_at(seq, args, 10.0f);
    if (!expect_voltage(seq, 1.0f, "first clock")) return 3;
    if (seq.outputs[L::GATE_OUTPUT].getVoltage() < 5.0f) {
        std::fprintf(stderr, "first clock: gate did not fire\n");
        return 4;
    }

    for (int step = 2; step <= SEQModule::kSteps; ++step) {
        if (!clock(seq, args, static_cast<float>(step), "clock sequence"))
            return 4 + step;
    }
    if (!clock(seq, args, 1.0f, "wrap to Step 1")) return 13;

    // Move away from Step 1, then reset without a simultaneous clock.
    if (!clock(seq, args, 2.0f, "before reset")) return 14;
    process_at(seq, args, 0.0f, 10.0f);
    if (!expect_voltage(seq, 1.0f, "reset alone")) return 15;
    process_at(seq, args, 0.0f, 0.0f);
    if (!clock(seq, args, 1.0f, "first clock after reset")) return 16;
    if (seq.outputs[L::GATE_OUTPUT].getVoltage() < 5.0f) {
        std::fprintf(stderr, "first clock after reset: gate did not fire\n");
        return 17;
    }

    return 0;
}
