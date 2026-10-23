// Multirate FDN reverb — the block engine against its reference behaviour.
//
// The engine runs a control period at a time: per-line block reads and
// diffusion, the cross-line stages as four-lane vectors, single-precision
// state where double precision bought nothing audible. None of that may change
// what the reverb does. These cases pin it three ways:
//
//  1. An output fingerprint: the 40 ms RMS envelope of the impulse response,
//     per rail, for three voicings, recorded from a sample-at-a-time
//     evaluation of the same DSP graph with double-precision filters and
//     per-sample sanitizing. The block engine reproduces it within 1e-4 dB;
//     the tolerance here is 0.01 dB, and a 2% decay change moves it by half a
//     decibel (the negative control).
//  2. Decay and density measured with support/reverb_metrics.hpp — band T60
//     and the echo-density mixing time — against the same reference, each with
//     a control that moves the metric past its tolerance.
//  3. Block-partition exactness for every mode, with and without flutter: the
//     recursions' cleanup runs on fixed sample grids, so how a host divides
//     its blocks is invisible to the bit.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/fdn_reverb.hpp>

#include "support/fdn_reverb_fixture.hpp"
#include "support/reverb_metrics.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

using pulp::signal::FdnReverb;
namespace fdn = pulp::signal::fdn;
using Param = FdnReverb::Param;
using Mode = FdnReverb::Mode;
using pulp::test::audio::band_t60;
using pulp::test::audio::echo_density_curve;
using pulp::test::audio::mixing_time_seconds;
using pulp::test::audio::range_rms;
using namespace pulp::test::fdn_reverb;

namespace {

constexpr int kWindows = 30; // 40 ms each, 0 .. 1.2 s
constexpr double kWindowSeconds = 0.040;
constexpr double kEnvelopeToleranceDb = 0.01;

struct Reference {
    const char* name;
    std::array<double, kWindows> left;
    std::array<double, kWindows> right;
    double t60_1k;
    double t60_4k;
    double mixing;
};

// clang-format off
constexpr std::array<Reference, 3> kReference = {{
    {"defaults",
     {-86.0635, -60.8697, -58.0579, -57.6101, -58.6271, -60.0386,
      -61.3602, -62.7121, -64.3461, -64.9686, -65.9815, -67.4603,
      -68.7684, -70.0819, -70.3319, -72.0569, -73.2883, -73.1566,
      -74.9953, -75.9709, -76.6617, -77.7147, -79.8440, -80.2176,
      -81.7246, -82.0417, -82.4775, -85.4080, -85.0461, -86.0980},
     {-87.4370, -61.1748, -58.9132, -58.4349, -58.8774, -59.3904,
      -60.6063, -61.9091, -63.4132, -64.8904, -65.8949, -66.6812,
      -68.1989, -69.6641, -70.7171, -71.3709, -72.5415, -73.6508,
      -75.2096, -75.2920, -77.0593, -78.4080, -78.5720, -80.4733,
      -81.5873, -82.2325, -82.8428, -84.2651, -85.1228, -86.6289},
     2.46156, 1.92750, 0.05867},
    {"hall",
     {-87.2395, -60.5976, -57.4935, -57.6630, -58.1054, -59.7763,
      -61.3338, -63.0358, -63.7530, -64.6895, -65.5044, -67.1756,
      -68.3692, -69.4154, -70.3264, -70.8283, -72.0812, -73.0347,
      -74.3314, -75.0625, -75.3326, -76.7859, -77.7553, -78.8093,
      -79.5213, -80.6963, -80.8949, -81.8372, -82.8009, -83.7607},
     {-89.0481, -61.1378, -58.0241, -57.8882, -58.1487, -60.0952,
      -61.7740, -62.3581, -64.2313, -65.1428, -65.9479, -67.3371,
      -68.8462, -69.2796, -70.6474, -71.2644, -71.6226, -73.2433,
      -73.9547, -75.0052, -75.3030, -76.1935, -77.7110, -78.8591,
      -79.8520, -80.4265, -81.6630, -81.7469, -82.8858, -84.1623},
     2.84844, 2.10344, 0.07200},
    {"neutral at 48 kHz",
     {-70.1318, -54.8653, -53.4496, -52.9957, -53.4083, -54.7293,
      -55.8371, -56.6769, -57.5360, -58.2193, -59.2626, -60.1829,
      -61.4211, -62.1032, -62.7465, -63.7576, -64.5211, -65.8330,
      -66.6616, -67.7327, -68.5136, -69.6110, -70.5451, -71.3511,
      -72.2540, -73.4742, -74.2806, -75.2056, -76.0644, -77.3377},
     {-71.3271, -55.7308, -53.6240, -53.2448, -53.5197, -54.7870,
      -55.4919, -56.4915, -57.3997, -58.1665, -59.3982, -59.9671,
      -60.9992, -61.9825, -62.8987, -63.7313, -64.6307, -65.8498,
      -66.7075, -67.5477, -68.3903, -69.6236, -70.3704, -71.2100,
      -72.5625, -73.1030, -74.1067, -75.4661, -76.2582, -77.0998},
     2.51962, 2.50413, 0.07733},
}};
// clang-format on

// The neutral voicing of the third reference, with a decay and diffusion the
// controls can move.
void configure_reference_neutral(FdnReverb& reverb, double decay, double diffusion) {
    reverb.set_parameter(Param::decay, decay);
    reverb.set_parameter(Param::predelay, 0.0);
    reverb.set_parameter(Param::damp_hi, 0.0);
    reverb.set_parameter(Param::damp_lo, 0.0);
    reverb.set_parameter(Param::mod, 0.0);
    reverb.set_parameter(Param::diffusion, diffusion);
    reverb.set_parameter(Param::tank_rate, 5.0);
    reverb.snap_parameters();
    reverb.reset();
}

Stereo render_reference(std::size_t index, double decay = 2.5, double diffusion = 0.7) {
    FdnReverb reverb;
    reverb.prepare(kHostRate, kBlock);
    if (index == 1)
        reverb.set_mode(Mode::hall);
    if (index == 2)
        configure_reference_neutral(reverb, decay, diffusion);
    reverb.snap_parameters();
    reverb.reset();
    return render(reverb, impulse(static_cast<std::size_t>(kHostRate * 3.0)));
}

std::array<double, kWindows> envelope(const std::vector<float>& x) {
    std::array<double, kWindows> db{};
    const auto w = static_cast<std::size_t>(kHostRate * kWindowSeconds);
    for (std::size_t i = 0; i < db.size(); ++i)
        db[i] = 20.0 * std::log10(range_rms(x, i * w, (i + 1) * w) + 1e-30);
    return db;
}

double worst_envelope_difference(const std::vector<float>& x,
                                 const std::array<double, kWindows>& reference) {
    const auto measured = envelope(x);
    double worst = 0.0;
    for (std::size_t i = 0; i < measured.size(); ++i)
        worst = std::max(worst, std::abs(measured[i] - reference[i]));
    return worst;
}

} // namespace

TEST_CASE("fdn reverb block engine reproduces the reference output envelope",
          "[fdn][reverb][regression]") {
    for (std::size_t r = 0; r < kReference.size(); ++r) {
        const auto out = render_reference(r);
        const double left = worst_envelope_difference(out.left, kReference[r].left);
        const double right = worst_envelope_difference(out.right, kReference[r].right);
        INFO(kReference[r].name << ": worst 40 ms envelope difference L " << left << " dB, R "
                                << right << " dB");
        REQUIRE(left < kEnvelopeToleranceDb);
        REQUIRE(right < kEnvelopeToleranceDb);
    }
}

TEST_CASE("fdn reverb envelope fingerprint detects a two percent decay change",
          "[fdn][reverb][regression]") {
    // Negative control: if this stayed inside the tolerance, the fingerprint
    // above could not see a change to the decay law either.
    const auto out = render_reference(2, 2.45);
    const double left = worst_envelope_difference(out.left, kReference[2].left);
    INFO("worst envelope difference at decay 2.45 s: " << left << " dB");
    REQUIRE(left > 20.0 * kEnvelopeToleranceDb);
}

TEST_CASE("fdn reverb block engine keeps the reference decay and echo density",
          "[fdn][reverb][regression][decay][density]") {
    for (std::size_t r = 0; r < kReference.size(); ++r) {
        const auto out = render_reference(r);
        const double t1k = band_t60(out.left, kHostRate, 1000.0);
        const double t4k = band_t60(out.left, kHostRate, 4000.0);
        const double mixing = mixing_time_seconds(out.left, kHostRate, 0.9);
        INFO(kReference[r].name << ": T60 1 kHz " << t1k << " s (reference " << kReference[r].t60_1k
                                << "), 4 kHz " << t4k << " s (" << kReference[r].t60_4k
                                << "), mixing " << mixing * 1000.0 << " ms ("
                                << kReference[r].mixing * 1000.0 << ")");
        REQUIRE(std::abs(t1k - kReference[r].t60_1k) / kReference[r].t60_1k < 0.005);
        REQUIRE(std::abs(t4k - kReference[r].t60_4k) / kReference[r].t60_4k < 0.005);
        // One analysis hop of the echo-density curve.
        REQUIRE(std::abs(mixing - kReference[r].mixing) <= 128.0 / kHostRate + 1e-9);
    }
}

TEST_CASE("fdn reverb decay and density metrics move when the reverb does",
          "[fdn][reverb][regression][decay][density]") {
    // Negative controls for the case above, on the neutral reference: a 2%
    // shorter decay must leave the 0.5% T60 band, and removing diffusion must
    // move the mixing time by far more than one hop and empty the early
    // echo-density curve.
    const auto shorter = render_reference(2, 2.45);
    const double t1k = band_t60(shorter.left, kHostRate, 1000.0);
    INFO("T60 at decay 2.45 s: " << t1k << " s vs reference " << kReference[2].t60_1k);
    REQUIRE(std::abs(t1k - kReference[2].t60_1k) / kReference[2].t60_1k > 0.005);

    const auto sparse = render_reference(2, 2.5, 0.0);
    const double mixing = mixing_time_seconds(sparse.left, kHostRate, 0.9);
    INFO("mixing time without diffusion: " << mixing * 1000.0 << " ms");
    REQUIRE(mixing > kReference[2].mixing + 0.05);
    const auto dense_curve = echo_density_curve(render_reference(2).left, kHostRate);
    const auto sparse_curve = echo_density_curve(sparse.left, kHostRate);
    REQUIRE(dense_curve[3] > 0.1);
    REQUIRE(sparse_curve[3] < dense_curve[3] * 0.5);
}

TEST_CASE("fdn reverb block engine is exact across host block partitions",
          "[fdn][reverb][determinism]") {
    const auto n = static_cast<std::size_t>(kHostRate * 1.5);
    auto input = noise(n, 0.3f, 4711u);
    std::fill(input.begin() + static_cast<std::ptrdiff_t>(kHostRate * 0.1), input.end(), 0.0f);

    for (int mode = 0; mode < fdn::kNumModes; ++mode) {
        for (bool flutter : {false, true}) {
            auto render_with = [&](int block) {
                FdnReverb reverb;
                reverb.prepare(kHostRate, 4096);
                reverb.set_mode(static_cast<Mode>(mode));
                reverb.set_flutter(flutter);
                reverb.snap_parameters();
                reverb.reset();
                return render(reverb, input, block);
            };
            const auto whole = render_with(4096);
            INFO("mode " << fdn::kModeConfigs[static_cast<std::size_t>(mode)].name
                         << (flutter ? " with flutter" : ""));
            for (int block : {1, 37, 512}) {
                const auto split = render_with(block);
                INFO("block " << block);
                REQUIRE(split.left == whole.left);
                REQUIRE(split.right == whole.right);
            }
        }
    }
}
