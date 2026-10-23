// Classic reverb tier — the comb/allpass engine behind fdn::Tier::classic.
//
// Decay and density are measured with support/reverb_metrics.hpp, each with a
// control that must move the metric: band T60 against the requested decay,
// and the echo-density mixing time against the same tier with its allpass
// diffusion removed.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/classic_reverb.hpp>
#include <pulp/signal/fdn_reverb.hpp>

#include "support/fdn_reverb_fixture.hpp"
#include "support/reverb_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#define PULP_CLASSIC_HAS_INSTRUCTION_COUNT 1
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define PULP_CLASSIC_INSTRUMENTED 1
#endif
#endif

using pulp::signal::ClassicReverb;
using pulp::signal::FdnReverb;
namespace fdn = pulp::signal::fdn;
using Param = fdn::Param;
using pulp::test::audio::band_t60;
using pulp::test::audio::echo_density_curve;
using pulp::test::audio::mixing_time_seconds;
using pulp::test::audio::range_rms;
using namespace pulp::test::fdn_reverb;

namespace {

Stereo render_classic(ClassicReverb& reverb, const std::vector<float>& input, int block = kBlock) {
    Stereo out;
    out.left.assign(input.size(), 0.0f);
    out.right.assign(input.size(), 0.0f);
    for (std::size_t offset = 0; offset < input.size(); offset += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(
            std::min<std::size_t>(static_cast<std::size_t>(block), input.size() - offset));
        reverb.process_block(input.data() + offset, input.data() + offset, out.left.data() + offset,
                             out.right.data() + offset, n);
    }
    return out;
}

void configure(ClassicReverb& reverb, double decay, double damp_hi = 0.0, double diffusion = 0.7) {
    reverb.prepare(kHostRate, kBlock);
    reverb.set_parameter(Param::decay, decay);
    reverb.set_parameter(Param::damp_hi, damp_hi);
    reverb.set_parameter(Param::diffusion, diffusion);
    reverb.set_parameter(Param::predelay, 0.0);
    reverb.snap_parameters();
    reverb.reset();
}

// Energy at the update-rate sidebands of a 440 Hz probe, relative to the
// probe, in dB. A time-invariant reverb returns the probe; a control that
// moves in steps every `rate_hz` modulates it and puts energy at
// k * rate_hz +/- 440 Hz. Four-term Blackman-Harris windows (side lobes near
// -92 dB) of 16384 samples, half overlapped, from `start` for `length`
// samples; each sideband is summed over the bins within 12 Hz of it.
double sideband_db(const std::vector<float>& y, std::size_t start, std::size_t length,
                   double rate_hz) {
    constexpr std::size_t kWindow = 16384;
    constexpr double kProbeHz = 440.0;
    constexpr double kTwoPi = 6.283185307179586;
    std::vector<double> window(kWindow);
    for (std::size_t i = 0; i < kWindow; ++i) {
        const double x = kTwoPi * static_cast<double>(i) / static_cast<double>(kWindow - 1);
        window[i] =
            0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
    }
    const double bin_hz = kHostRate / static_cast<double>(kWindow);
    const auto energy_near = [&](std::size_t at, double hz) {
        double sum = 0.0;
        const auto lo = static_cast<long>(std::ceil((hz - 12.0) / bin_hz));
        const auto hi = static_cast<long>(std::floor((hz + 12.0) / bin_hz));
        for (long b = lo; b <= hi; ++b) {
            // The bin's phasor advanced by rotation rather than a sin/cos per
            // sample; over one window it drifts by far less than the floor.
            const double w = kTwoPi * static_cast<double>(b) / static_cast<double>(kWindow);
            const double step_re = std::cos(w), step_im = -std::sin(w);
            double ph_re = 1.0, ph_im = 0.0, re = 0.0, im = 0.0;
            for (std::size_t i = 0; i < kWindow; ++i) {
                const double v = window[i] * static_cast<double>(y[at + i]);
                re += v * ph_re;
                im += v * ph_im;
                const double next = ph_re * step_re - ph_im * step_im;
                ph_im = ph_re * step_im + ph_im * step_re;
                ph_re = next;
            }
            sum += re * re + im * im;
        }
        return sum;
    };
    double sidebands = 0.0, probe = 0.0;
    for (std::size_t at = start; at + kWindow <= start + length; at += kWindow / 2) {
        probe += energy_near(at, kProbeHz);
        for (int k = 1; k <= 4; ++k)
            for (const double hz : {k * rate_hz - kProbeHz, k * rate_hz + kProbeHz})
                sidebands += energy_near(at, hz);
    }
    return 10.0 * std::log10(sidebands / probe + 1e-30);
}

#if defined(PULP_CLASSIC_HAS_INSTRUCTION_COUNT)
// Instructions this process has retired, or 0 where the kernel does not count
// them (some virtual machines report nothing).
std::uint64_t retired_instructions() {
    rusage_info_v4 info{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) != 0)
        return 0;
    return info.ri_instructions;
}
#endif

} // namespace

TEST_CASE("classic reverb realizes the requested T60", "[reverb][classic][decay]") {
    for (double decay : {0.5, 1.3, 3.0}) {
        ClassicReverb reverb;
        configure(reverb, decay);
        const auto out = render_classic(
            reverb, impulse(static_cast<std::size_t>(kHostRate * (decay * 2.5 + 1.0))));
        const double measured = band_t60(out.left, kHostRate, kDecayProbeHz);
        INFO("decay " << decay << " s measured " << measured << " s");
        REQUIRE(measured > 0.0);
        REQUIRE(std::abs(measured - decay) / decay < 0.10);
    }
}

TEST_CASE("classic reverb T60 measurement follows the decay control", "[reverb][classic][decay]") {
    // Negative control: the measurement above must be able to fail. A decay
    // 30% shorter has to land outside the 10% band around the longer one.
    ClassicReverb longer;
    configure(longer, 1.3);
    ClassicReverb shorter;
    configure(shorter, 0.9);
    const auto probe = impulse(static_cast<std::size_t>(kHostRate * 4.5));
    const double t_long = band_t60(render_classic(longer, probe).left, kHostRate, kDecayProbeHz);
    const double t_short = band_t60(render_classic(shorter, probe).left, kHostRate, kDecayProbeHz);
    INFO("T60 at 1.3 s " << t_long << ", at 0.9 s " << t_short);
    REQUIRE(t_short < t_long * 0.85);
}

TEST_CASE("classic reverb damping shortens the high band", "[reverb][classic][damping]") {
    auto band_times = [](double damp_hi) {
        ClassicReverb reverb;
        configure(reverb, 2.0, damp_hi);
        const auto out = render_classic(reverb, impulse(static_cast<std::size_t>(kHostRate * 6)));
        return std::pair{band_t60(out.left, kHostRate, 500.0),
                         band_t60(out.left, kHostRate, 8000.0)};
    };
    const auto [flat_low, flat_high] = band_times(0.0);
    const auto [damped_low, damped_high] = band_times(0.6);
    INFO("flat 500 Hz " << flat_low << " / 8 kHz " << flat_high << "; damped " << damped_low
                        << " / " << damped_high);
    REQUIRE(std::abs(flat_high - flat_low) / flat_low < 0.20);
    REQUIRE(damped_high < damped_low * 0.6);
}

TEST_CASE("classic reverb diffusion sets its echo density", "[reverb][classic][density]") {
    // The allpasses are what fill in between the comb echoes: with them (and
    // the default in-loop damping, which smears each recirculation) the
    // response reaches a Gaussian-like density, without them it stays sparse.
    // The second render is the control that keeps the first from passing on a
    // metric that cannot tell the two apart.
    ClassicReverb dense;
    configure(dense, 1.3, 0.4);
    ClassicReverb sparse;
    configure(sparse, 1.3, 0.4, 0.0);
    const auto probe = impulse(static_cast<std::size_t>(kHostRate * 2));
    const auto dense_out = render_classic(dense, probe);
    const auto sparse_out = render_classic(sparse, probe);
    const double mixing = mixing_time_seconds(dense_out.left, kHostRate, 0.9);
    const double sparse_mixing = mixing_time_seconds(sparse_out.left, kHostRate, 0.9);
    INFO("mixing time " << mixing * 1000.0 << " ms, without diffusion " << sparse_mixing * 1000.0
                        << " ms");
    REQUIRE(mixing >= 0.0);
    REQUIRE(mixing < 0.25);
    REQUIRE((sparse_mixing < 0.0 || sparse_mixing > mixing + 0.05));
    const auto dense_curve = echo_density_curve(dense_out.left, kHostRate);
    const auto sparse_curve = echo_density_curve(sparse_out.left, kHostRate);
    REQUIRE(dense_curve[12] > sparse_curve[12] * 1.5);
}

TEST_CASE("classic reverb combs follow the absorptive decay law", "[reverb][classic][structure]") {
    ClassicReverb reverb;
    configure(reverb, 2.0);
    std::set<int> lengths;
    for (int c = 0; c < pulp::signal::classic_reverb::kCombs; ++c) {
        const int length = reverb.comb_length(c);
        lengths.insert(length);
        const double expected = std::pow(10.0, -3.0 * length / (2.0 * kHostRate));
        INFO("comb " << c << " length " << length);
        REQUIRE(std::abs(reverb.comb_gain(c) - expected) < 1e-12);
        REQUIRE(reverb.comb_gain(c) < 1.0);
    }
    REQUIRE(lengths.size() == static_cast<std::size_t>(pulp::signal::classic_reverb::kCombs));
}

TEST_CASE("classic reverb rails are decorrelated", "[reverb][classic][stereo]") {
    ClassicReverb reverb;
    configure(reverb, 2.0);
    const auto out = render_classic(reverb, impulse(static_cast<std::size_t>(kHostRate)));
    const auto from = static_cast<std::size_t>(kHostRate * 0.1);
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (std::size_t i = from; i < out.left.size(); ++i) {
        xy += static_cast<double>(out.left[i]) * out.right[i];
        xx += static_cast<double>(out.left[i]) * out.left[i];
        yy += static_cast<double>(out.right[i]) * out.right[i];
    }
    const double correlation = xy / std::sqrt(xx * yy);
    INFO("tail correlation " << correlation);
    REQUIRE(std::abs(correlation) < 0.5);
}

TEST_CASE("classic reverb is exact across host block partitions",
          "[reverb][classic][determinism]") {
    auto input = noise(static_cast<std::size_t>(kHostRate * 1.5), 0.3f, 99u);
    std::fill(input.begin() + static_cast<std::ptrdiff_t>(kHostRate * 0.1), input.end(), 0.0f);
    auto render_with = [&](int block) {
        ClassicReverb reverb;
        configure(reverb, 2.5, 0.4);
        // A live change, smoothed across blocks, to a decay short enough that
        // the tail reaches the snap threshold inside the render: the cleanup
        // grid is then part of what must not depend on the partition.
        reverb.set_parameter(Param::decay, 0.12);
        return render_classic(reverb, input, block);
    };
    const auto whole = render_with(4096);
    for (int block : {1, 37, 512}) {
        const auto split = render_with(block);
        INFO("block " << block);
        REQUIRE(split.left == whole.left);
        REQUIRE(split.right == whole.right);
    }
}

TEST_CASE("classic reverb is true stereo", "[reverb][classic][stereo]") {
    // Each rail feeds its own comb bank, so an anti-phase input — which a
    // mono-summing input would cancel to silence — still reverberates, and a
    // left-only input reaches the left output first.
    ClassicReverb reverb;
    configure(reverb, 1.5);
    const auto n = static_cast<std::size_t>(kHostRate);
    std::vector<float> left(n, 0.0f), right(n, 0.0f), out_l(n), out_r(n);
    left[0] = 1.0f;
    right[0] = -1.0f;
    reverb.process_block(left.data(), right.data(), out_l.data(), out_r.data(),
                         static_cast<int>(n));
    REQUIRE(range_rms(out_l, 0, n) > 1e-4);
    REQUIRE(range_rms(out_r, 0, n) > 1e-4);
}

TEST_CASE("classic reverb stays bounded and recovers from non-finite input",
          "[reverb][classic][stability]") {
    Rng rng(0xC1A551Cu);
    for (int vector = 0; vector < 24; ++vector) {
        ClassicReverb reverb;
        reverb.prepare(kHostRate, kBlock);
        for (int p = 0; p < fdn::kNumParams; ++p) {
            const auto& spec = fdn::kParamSpecs[static_cast<std::size_t>(p)];
            reverb.set_parameter(static_cast<Param>(p),
                                 spec.min + rng.unit() * (spec.max - spec.min));
        }
        if (vector % 2 == 0)
            reverb.set_parameter(Param::decay, 60.0);
        reverb.snap_parameters();
        reverb.reset();
        auto input = noise(static_cast<std::size_t>(kHostRate * 2.0), 0.5f, rng.next_u32());
        input[1000] = std::numeric_limits<float>::quiet_NaN();
        input[2000] = std::numeric_limits<float>::infinity();
        const auto out = render_classic(reverb, input);
        INFO("vector " << vector);
        REQUIRE(all_finite(out));
        REQUIRE(peak(out) < fdn::kSanityCeil);
    }
}

TEST_CASE("fdn reverb classic tier runs the classic engine behind the same controls",
          "[reverb][classic][tier]") {
    const auto input = noise(static_cast<std::size_t>(kHostRate * 0.5), 0.3f, 5u);

    FdnReverb engine;
    engine.prepare(kHostRate, kBlock);
    engine.set_mode(FdnReverb::Mode::hall);
    engine.set_parameter(Param::predelay, 12.0);
    engine.set_tier(fdn::Tier::classic);
    engine.snap_parameters();
    engine.reset();
    REQUIRE(engine.tier() == fdn::Tier::classic);
    const auto tiered = render(engine, input);

    ClassicReverb classic;
    classic.prepare(kHostRate, kBlock);
    classic.set_mode(FdnReverb::Mode::hall);
    classic.set_parameter(Param::predelay, 12.0);
    classic.snap_parameters();
    classic.reset();
    const auto direct = render_classic(classic, input);
    REQUIRE(tiered.left == direct.left);
    REQUIRE(tiered.right == direct.right);

    // Switching back is a clean flush onto the network: the next render is
    // exactly a fresh network engine's.
    engine.set_tier(fdn::Tier::network);
    const auto back = render(engine, input);
    FdnReverb fresh;
    fresh.prepare(kHostRate, kBlock);
    fresh.set_mode(FdnReverb::Mode::hall);
    fresh.set_parameter(Param::predelay, 12.0);
    fresh.snap_parameters();
    fresh.reset();
    const auto network = render(fresh, input);
    REQUIRE(back.left == network.left);
    REQUIRE(back.right == network.right);
    REQUIRE(back.left != tiered.left);
}

// The comb bank is the tier's whole cost. Run as four-lane vectors over
// interleaved rings it retires about 140 instructions per stereo frame on
// arm64; sixteen scalar combs over separate rings retire about 455. A retired
// instruction count does not depend on host load the way wall time does, so
// the budget can sit between the two and fail only on a real regression.
TEST_CASE("classic reverb comb bank stays within its instruction budget",
          "[reverb][classic][throughput]") {
#if !defined(PULP_CLASSIC_HAS_INSTRUCTION_COUNT)
    SKIP("retired-instruction counts are read on arm64 macOS only");
#elif !defined(NDEBUG) || defined(PULP_CLASSIC_INSTRUMENTED)
    SKIP("the instruction budget applies to optimized, uninstrumented builds");
#else
    constexpr int kFrames = 128;
    constexpr int kBlocks = 2000;
    constexpr double kBudgetPerFrame = 260.0;

    ClassicReverb reverb;
    configure(reverb, 1.34, 0.4);
    std::vector<float> left(kFrames), right(kFrames), out_left(kFrames), out_right(kFrames);
    for (int k = 0; k < kFrames; ++k) {
        left[static_cast<std::size_t>(k)] = 0.001f * static_cast<float>(k & 31);
        right[static_cast<std::size_t>(k)] = -left[static_cast<std::size_t>(k)];
    }
    const auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b)
            reverb.process_block(left.data(), right.data(), out_left.data(), out_right.data(),
                                 kFrames);
    };
    run(100);

    const std::uint64_t before = retired_instructions();
    run(kBlocks);
    const std::uint64_t after = retired_instructions();
    if (before == 0 || after <= before)
        SKIP("this host does not report retired instructions");

    const double per_frame =
        static_cast<double>(after - before) / (static_cast<double>(kBlocks) * kFrames);
    INFO("classic reverb retired " << per_frame << " instructions per stereo frame");
    // A count far below the work the bank must do means the counter is not
    // measuring this loop.
    REQUIRE(per_frame > 40.0);
    REQUIRE(per_frame < kBudgetPerFrame);
#endif
}

// A host that automates a control sets a new target once per block. The
// tier moves its comb gains and damping toward each control tick's values in
// per-sample steps, and smooths targets through three stages, so a sweep
// leaves no energy at the 32-sample control rate or at the host's block rate
// that a stationary render does not also show.
TEST_CASE("classic reverb sweeps its controls without zipper noise",
          "[reverb][classic][automation]") {
    constexpr int kHostBlock = 128;
    constexpr std::size_t kSettle = 6 * 48000;
    constexpr std::size_t kSweep = 2 * 48000;
    std::vector<float> probe(kSettle + kSweep);
    for (std::size_t i = 0; i < probe.size(); ++i)
        probe[i] = 0.25f * static_cast<float>(std::sin(6.283185307179586 * 440.0 *
                                                       static_cast<double>(i) / kHostRate));
    const auto render_sweep = [&](Param param, double from, double to) {
        ClassicReverb reverb;
        reverb.prepare(kHostRate, kHostBlock);
        reverb.set_parameter(Param::predelay, 0.0);
        reverb.set_parameter(Param::decay, 0.8);
        reverb.set_parameter(param, from);
        reverb.snap_parameters();
        reverb.reset();
        std::vector<float> left(probe.size()), right(probe.size());
        for (std::size_t at = 0; at < probe.size(); at += kHostBlock) {
            const double t = at < kSettle
                                 ? 0.0
                                 : static_cast<double>(at - kSettle) / static_cast<double>(kSweep);
            reverb.set_parameter(param, from + (to - from) * t);
            reverb.process_block(probe.data() + at, probe.data() + at, left.data() + at,
                                 right.data() + at, kHostBlock);
        }
        return left;
    };
    const double control_hz = kHostRate / fdn::kControlRateSamples;
    const double block_hz = kHostRate / kHostBlock;

    const auto still = render_sweep(Param::damp_hi, 0.3, 0.3);
    const double floor_control = sideband_db(still, kSettle, kSweep, control_hz);
    const double floor_block = sideband_db(still, kSettle, kSweep, block_hz);
    INFO("stationary floor " << floor_control << " / " << floor_block << " dB");
    REQUIRE(floor_control < -115.0);
    REQUIRE(floor_block < -110.0);

    // Control: the same measurement sees a probe whose gain climbs in steps
    // of 0.01% every 32 samples, a staircase like an unramped control's.
    std::vector<float> stepped(probe.size());
    for (std::size_t i = 0; i < probe.size(); ++i)
        stepped[i] = probe[i] * (1.0f + 1e-4f * static_cast<float>(i / 32 % 4096));
    REQUIRE(sideband_db(stepped, kSettle, kSweep, control_hz) > -100.0);

    for (const auto& [param, from, to] :
         {std::tuple{Param::damp_hi, 0.0, 1.0}, std::tuple{Param::decay, 0.8, 4.0}}) {
        const auto swept = render_sweep(param, from, to);
        const double at_control = sideband_db(swept, kSettle, kSweep, control_hz);
        const double at_block = sideband_db(swept, kSettle, kSweep, block_hz);
        INFO("param " << static_cast<int>(param) << ": " << at_control << " / " << at_block
                      << " dB");
        REQUIRE(at_control < floor_control + 3.0);
        REQUIRE(at_block < floor_block + 3.0);
    }
}
