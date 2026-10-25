// Renders the FDN reverb's impulse-response fingerprints: the 40 ms RMS
// envelope per rail over 0 .. 1.2 s, for every voicing the block-engine test
// pins. Built against any engine with the public FdnReverb API, so the same
// source records the reference from a released engine and measures the
// current one. Printed as C++ initializers for test_fdn_reverb_block_engine.cpp.
//
// Voicing indices must stay in step with the test's kReference table.

#include <pulp/signal/fdn_reverb.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using Reverb = pulp::signal::FdnReverb;
using Param = Reverb::Param;
using Mode = Reverb::Mode;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kWindows = 30;
constexpr double kWindowSeconds = 0.040;

struct Voicing {
    const char* name;
    void (*configure)(Reverb&);
};

void neutral(Reverb& r) {
    r.set_parameter(Param::decay, 2.5);
    r.set_parameter(Param::predelay, 0.0);
    r.set_parameter(Param::damp_hi, 0.0);
    r.set_parameter(Param::damp_lo, 0.0);
    r.set_parameter(Param::mod, 0.0);
    r.set_parameter(Param::diffusion, 0.7);
    r.set_parameter(Param::tank_rate, 5.0);
}

const Voicing kVoicings[] = {
    {"defaults", [](Reverb&) {}},
    {"hall", [](Reverb& r) { r.set_mode(Mode::hall); }},
    {"neutral at 48 kHz", neutral},
    {"room", [](Reverb& r) { r.set_mode(Mode::room); }},
    {"galaxy", [](Reverb& r) { r.set_mode(Mode::galaxy); }},
    {"shimmer", [](Reverb& r) { r.set_mode(Mode::shimmer); }},
    {"lofi", [](Reverb& r) { r.set_mode(Mode::lofi); }},
    {"defaults driven", [](Reverb& r) { r.set_parameter(Param::drive, 0.7); }},
    {"defaults with flutter", [](Reverb& r) { r.set_flutter(true); }},
};

double rms(const std::vector<float>& x, std::size_t begin, std::size_t end) {
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
}

void print_envelope(const std::vector<float>& x) {
    const auto w = static_cast<std::size_t>(kRate * kWindowSeconds);
    std::printf("     {");
    for (int i = 0; i < kWindows; ++i) {
        const double db = 20.0 * std::log10(rms(x, static_cast<std::size_t>(i) * w,
                                                static_cast<std::size_t>(i + 1) * w) +
                                            1e-30);
        std::printf("%.4f%s", db, i + 1 < kWindows ? (i % 6 == 5 ? ",\n      " : ", ") : "}");
    }
}

} // namespace

int main() {
    const auto n = static_cast<std::size_t>(kRate * 3.0);
    for (const auto& v : kVoicings) {
        Reverb reverb;
        reverb.prepare(kRate, kBlock);
        v.configure(reverb);
        reverb.snap_parameters();
        reverb.reset();
        std::vector<float> in(n, 0.0f), left(n), right(n);
        in[0] = 1.0f;
        for (std::size_t offset = 0; offset < n; offset += kBlock) {
            const int m = static_cast<int>(std::min<std::size_t>(kBlock, n - offset));
            reverb.process_block(in.data() + offset, in.data() + offset, left.data() + offset,
                                 right.data() + offset, m);
        }
        std::printf("    {\"%s\",\n", v.name);
        print_envelope(left);
        std::printf(",\n");
        print_envelope(right);
        std::printf("},\n");
    }
}
