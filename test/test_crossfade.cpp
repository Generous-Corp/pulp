// SF-2 crossfade unification — the ONE fixture covering every SIGNAL-side fade.
//
// After SF-2 there is a single old->new crossfade law (signal::crossfade_gains
// over an optional signal::crossfade_smoothstep shaping). This fixture proves:
//   1. the shared law's invariants (both gain laws + the smoothstep ramp);
//   2. signal::TransitionMixer (the live plugin swap + convolver IR swap) blends
//      through it — float AND double, both curves;
//   3. the live_kernel structural-swap fade blends through it (matching native);
//   4. the audio LoopRenderer wrap-crossfade blends through it.
//   5. ProcessingSwitchCrossfade (a switch between realisations whose
//      latencies differ) warms, then fades through TransitionMixer EqualPower.
// The PartitionedConvolver IR-swap fade blends via signal::TransitionMixer (see
// §2 + test_convolver_bg_swap), so it shares the identical law by construction.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/processing_switch_crossfade.hpp>
#include <pulp/signal/transition_mixer.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>

#include <live_kernel/crossfade.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::CrossfadeGainLaw;
using pulp::signal::crossfade_gains;
using pulp::signal::crossfade_smoothstep;
using pulp::signal::TransitionCurve;
using pulp::signal::TransitionMixer;
using pulp::signal::TransitionMixer64;

// ── §1 — the shared law's invariants ────────────────────────────────────────

TEST_CASE("crossfade: smoothstep ramp is click-free (zero slope, symmetric)",
          "[signal][crossfade][sf2]") {
    REQUIRE(crossfade_smoothstep(0.0) == 0.0);
    REQUIRE(crossfade_smoothstep(1.0) == 1.0);
    REQUIRE_THAT(crossfade_smoothstep(0.5), WithinAbs(0.5, 1e-15));
    // Clamped outside [0,1].
    REQUIRE(crossfade_smoothstep(-0.3) == 0.0);
    REQUIRE(crossfade_smoothstep(1.7) == 1.0);
    // Symmetric: s(t) + s(1-t) == 1.
    for (double t = 0.0; t <= 1.0; t += 0.05) {
        REQUIRE_THAT(crossfade_smoothstep(t) + crossfade_smoothstep(1.0 - t),
                     WithinAbs(1.0, 1e-12));
    }
    // Zero slope at the ends (finite-difference derivative -> 0).
    const double h = 1e-6;
    REQUIRE(std::abs(crossfade_smoothstep(h) - crossfade_smoothstep(0.0)) / h < 1e-4);
    REQUIRE(std::abs(crossfade_smoothstep(1.0) - crossfade_smoothstep(1.0 - h)) / h < 1e-4);
}

TEST_CASE("crossfade: gain laws hit the right sums and endpoints",
          "[signal][crossfade][sf2]") {
    for (double u = 0.0; u <= 1.0; u += 0.05) {
        double og = 0.0, ng = 0.0;
        crossfade_gains(u, CrossfadeGainLaw::EqualGain, og, ng);
        REQUIRE_THAT(og + ng, WithinAbs(1.0, 1e-15));      // amplitude sum == 1
        crossfade_gains(u, CrossfadeGainLaw::EqualPower, og, ng);
        REQUIRE_THAT(og * og + ng * ng, WithinAbs(1.0, 1e-12));  // power sum == 1
    }
    // Endpoints: fully old at u=0, fully new at u=1.
    double og = 0.0, ng = 0.0;
    crossfade_gains(0.0, CrossfadeGainLaw::EqualPower, og, ng);
    REQUIRE_THAT(og, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(ng, WithinAbs(0.0, 1e-12));
    crossfade_gains(1.0, CrossfadeGainLaw::EqualPower, og, ng);
    REQUIRE_THAT(og, WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(ng, WithinAbs(1.0, 1e-12));
}

// ── §2 — TransitionMixer routes through the shared law ──────────────────────

TEST_CASE("crossfade: TransitionMixer gains equal the shared law (both curves)",
          "[signal][crossfade][sf2]") {
    const std::size_t len = 257;  // odd, so the midpoint is not a special case
    for (auto curve : {TransitionCurve::Smoothstep, TransitionCurve::EqualPower}) {
        const auto law = curve == TransitionCurve::EqualPower
                             ? CrossfadeGainLaw::EqualPower
                             : CrossfadeGainLaw::EqualGain;
        TransitionMixer mf;
        mf.configure(len, curve);
        TransitionMixer64 md;
        md.configure(len, curve);
        for (std::size_t p = 0; p <= len + 8; ++p) {
            float mof = 0.0f, mnf = 0.0f;
            mf.gains_at(p, mof, mnf);
            float sof = 0.0f, snf = 0.0f;
            crossfade_gains(crossfade_smoothstep(static_cast<float>(p) /
                                                 static_cast<float>(len)),
                            law, sof, snf);
            REQUIRE(mof == sof);  // bit-exact: the mixer runs the shared law
            REQUIRE(mnf == snf);

            double mod = 0.0, mnd = 0.0;
            md.gains_at(p, mod, mnd);
            double sod = 0.0, snd = 0.0;
            crossfade_gains(crossfade_smoothstep(static_cast<double>(p) /
                                                 static_cast<double>(len)),
                            law, sod, snd);
            REQUIRE(mod == sod);
            REQUIRE(mnd == snd);
        }
    }
}

// ── §3 — live_kernel structural swap routes through the shared law ──────────

TEST_CASE("crossfade: live_kernel fade equals the shared smoothstep equal-power law",
          "[signal][crossfade][sf2][live_kernel]") {
    const int fade_len = 480;
    const int n = 300;
    std::vector<float> ob(static_cast<std::size_t>(n), 1.0f);
    std::vector<float> nb(static_cast<std::size_t>(n), -1.0f);
    std::vector<float> got(static_cast<std::size_t>(n));
    // Straddle the fade end so the clamp region is exercised too.
    const int fade_pos = fade_len - 150;
    pulp::live_kernel::equal_power_fade_block(got.data(), ob.data(), nb.data(), n,
                                              fade_pos, fade_len);
    for (int i = 0; i < n; ++i) {
        float go = 0.0f, gn = 0.0f;
        crossfade_gains(crossfade_smoothstep(static_cast<float>(fade_pos + i) /
                                             static_cast<float>(fade_len)),
                        CrossfadeGainLaw::EqualPower, go, gn);
        const float expected = ob[static_cast<std::size_t>(i)] * go +
                               nb[static_cast<std::size_t>(i)] * gn;
        REQUIRE(got[static_cast<std::size_t>(i)] == expected);
    }
}

// ── §4 — LoopRenderer wrap-crossfade routes through the shared law ──────────

TEST_CASE("crossfade: LoopRenderer wrap blend equals the shared (raw-ramp) law",
          "[signal][crossfade][sf2][loop]") {
    using pulp::audio::Buffer;
    using pulp::audio::BufferView;
    using pulp::audio::LoopCrossfadeCurve;
    using pulp::audio::LoopInterpolationMode;
    using pulp::audio::LoopPlaybackMode;
    using pulp::audio::LoopReader;
    using pulp::audio::LoopRegion;
    using pulp::audio::LoopRenderer;

    constexpr std::size_t kChannels = 2;
    constexpr std::size_t kSourceFrames = 1100;
    constexpr std::uint64_t kStart = 100;
    constexpr std::uint64_t kEnd = 1000;
    constexpr std::uint32_t kXfade = 64;

    Buffer<float> source(kChannels, kSourceFrames);
    for (std::size_t i = 0; i < kSourceFrames; ++i) {
        source.channel(0)[i] = std::sin(0.017f * static_cast<float>(i)) * 0.9f;
        source.channel(1)[i] = std::cos(0.023f * static_cast<float>(i) + 0.5f) * 0.7f;
    }
    std::vector<const float*> ptrs(kChannels);
    for (std::size_t ch = 0; ch < kChannels; ++ch) ptrs[ch] = source.channel(ch).data();
    const BufferView<const float> input(ptrs.data(), kChannels, kSourceFrames);

    // The shared-law oracle for a forward loop's wrap crossfade: the loop uses
    // the RAW ramp (clamped t, no smoothstep), so the oracle shapes nothing.
    auto oracle = [&](const LoopRegion& region, std::uint32_t ch, double position) {
        const auto crossfade = static_cast<double>(region.crossfade_frames);
        const auto start = static_cast<double>(region.start_frame);
        const auto end = static_cast<double>(region.end_frame);
        const auto norm = LoopReader::normalize_position(region, position);
        const auto law = region.crossfade_curve == LoopCrossfadeCurve::EqualPower
                             ? CrossfadeGainLaw::EqualPower
                             : CrossfadeGainLaw::EqualGain;
        auto blended = [&](double t, double wrapped_pos) {
            const double u = std::clamp(t, 0.0, 1.0);
            double og = 0.0, ng = 0.0;
            crossfade_gains(u, law, og, ng);
            const double a = LoopReader::read_validated(input, region, ch, norm);
            const double b = LoopReader::read_validated(input, region, ch, wrapped_pos);
            return static_cast<float>(a * og + b * ng);
        };
        if (norm >= end - crossfade)
            return blended((norm - (end - crossfade)) / crossfade,
                           start + (norm - (end - crossfade)));
        return LoopReader::read_validated(input, region, ch, norm);
    };

    for (auto curve : {LoopCrossfadeCurve::EqualPower, LoopCrossfadeCurve::Linear}) {
        LoopRegion region;
        region.start_frame = kStart;
        region.end_frame = kEnd;
        region.crossfade_frames = kXfade;
        region.crossfade_curve = curve;
        region.playback_mode = LoopPlaybackMode::Forward;
        region.reverse_entry = false;
        region.interpolation = LoopInterpolationMode::None;
        region.source_sample_rate = 48000.0;

        LoopRenderer renderer;
        REQUIRE(renderer.set_region(region, kSourceFrames));
        renderer.set_playback_rate(1.0);
        renderer.start();

        Buffer<float> frame_out(kChannels, 1);
        bool wrap_seen = false;
        for (int f = 0; f < 1000; ++f) {
            const double pos_before = renderer.position();
            renderer.render(input, frame_out.view(), 1);
            for (std::uint32_t ch = 0; ch < kChannels; ++ch)
                REQUIRE(frame_out.channel(ch)[0] == oracle(region, ch, pos_before));
            const double norm = LoopReader::normalize_position(region, pos_before);
            if (norm >= static_cast<double>(kEnd) - static_cast<double>(kXfade))
                wrap_seen = true;
        }
        REQUIRE(wrap_seen);  // the crossfade region was actually exercised
    }
}

// ── §5 — a switch between realisations whose latencies differ ──────────────

namespace {

using pulp::signal::plan_processing_switch;
using pulp::signal::processing_switch_gains_at;
using pulp::signal::ProcessingSwitchCrossfade;
using pulp::signal::ProcessingSwitchPlan;

/// A realisation with `latency` samples of pure delay that starts, as a newly
/// built one does, with no history: its first `latency` outputs are silence.
struct DelayRealisation {
    std::vector<float> ring;
    std::size_t write = 0;
    explicit DelayRealisation(std::size_t latency) : ring(latency + 1, 0.0f) {}
    void process(const float* in, float* out, int n) {
        for (int i = 0; i < n; ++i) {
            ring[write] = in[i];
            write = (write + 1) % ring.size();
            out[i] = ring[write]; // the oldest sample: `latency` behind
        }
    }
};

struct SwitchMeasure {
    double worst_step = 0.0;    ///< largest |x[n] - x[n-1]| from the switch on
    double steady_step = 0.0;   ///< the same before the switch
    std::size_t silent_run = 0; ///< longest run of |x| < 1e-6 after the switch
};

/// Run a sine through `from`, switch to a fresh `to` at `switch_at`, either
/// as a cut or through the crossfade, in host blocks of `block`.
SwitchMeasure run_switch(std::size_t from_latency, std::size_t to_latency, bool crossfade,
                         int block) {
    constexpr double kRate = 48000.0;
    constexpr std::size_t kTotal = 48000, kSwitchAt = 16000;
    DelayRealisation from(from_latency), to(to_latency);
    ProcessingSwitchCrossfade xf;
    std::vector<float> in(kTotal), out(kTotal, 0.0f), incoming(static_cast<std::size_t>(block));
    for (std::size_t n = 0; n < kTotal; ++n)
        in[n] = static_cast<float>(
            0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * double(n) / kRate));
    bool switched = false, done = false;
    for (std::size_t pos = 0; pos < kTotal; pos += static_cast<std::size_t>(block)) {
        const int n =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), kTotal - pos));
        if (!switched && pos >= kSwitchAt) {
            switched = true;
            if (crossfade)
                xf.begin(
                    plan_processing_switch(static_cast<std::int64_t>(to_latency), 0, kRate, 0.02));
            else
                done = true; // a cut: the fresh realisation is heard at once
        }
        float* o = out.data() + pos;
        if (!switched || (crossfade && !done))
            from.process(in.data() + pos, o, n);
        if (switched && !done) {
            to.process(in.data() + pos, incoming.data(), n);
            float* op[] = {o};
            const float* ip[] = {incoming.data()};
            xf.mix(op, ip, 1, n);
            if (xf.finished())
                done = true;
        } else if (done) {
            to.process(in.data() + pos, o, n);
        }
    }
    SwitchMeasure m;
    for (std::size_t n = 1; n < kSwitchAt; ++n)
        m.steady_step = std::max(m.steady_step, std::abs(double(out[n]) - out[n - 1]));
    std::size_t run = 0;
    for (std::size_t n = kSwitchAt; n < kTotal; ++n) {
        m.worst_step = std::max(m.worst_step, std::abs(double(out[n]) - out[n - 1]));
        run = std::abs(out[n]) < 1e-6f ? run + 1 : 0;
        m.silent_run = std::max(m.silent_run, run);
    }
    return m;
}

} // namespace

TEST_CASE("processing switch: the plan is latency + history, then the fade",
          "[crossfade][processing-switch]") {
    const auto plan = plan_processing_switch(10240, 0, 48000.0, 0.03);
    REQUIRE(plan.warm_samples == 10240);
    REQUIRE(plan.fade_samples == 1440);
    REQUIRE(plan.total_samples() == 11680);
    const auto fir = plan_processing_switch(64, 8192, 96000.0, 0.03);
    REQUIRE(fir.warm_samples == 8256);
    REQUIRE(fir.fade_samples == 2880);
    // A degenerate request still ends on the incoming realisation.
    const auto none = plan_processing_switch(-5, -5, 0.0, -1.0);
    REQUIRE(none.warm_samples == 0);
    REQUIRE(none.fade_samples == 1);
}

TEST_CASE("processing switch: the schedule is warm, then TransitionMixer EqualPower",
          "[crossfade][processing-switch]") {
    const ProcessingSwitchPlan plan{100, 50};
    TransitionMixer reference;
    reference.configure(50, TransitionCurve::EqualPower);
    for (std::int64_t p = 0; p < 200; ++p) {
        float o = 0.0f, i = 0.0f;
        processing_switch_gains_at(plan, p, o, i);
        REQUIRE_THAT(double(o) * o + double(i) * i, WithinAbs(1.0, 1e-5));
        if (p < 100) {
            REQUIRE(o == 1.0f);
            REQUIRE(i == 0.0f);
        } else if (p < 150) {
            float ro = 0.0f, ri = 0.0f;
            reference.gains_at(static_cast<std::size_t>(p - 100), ro, ri);
            REQUIRE(o == ro);
            REQUIRE(i == ri);
        } else {
            REQUIRE(o == 0.0f);
            REQUIRE(i == 1.0f);
        }
    }
    // The stateful mixer applies exactly the pure schedule, across any block
    // partition.
    for (int block : {1, 7, 64}) {
        ProcessingSwitchCrossfade xf;
        xf.begin(plan);
        std::vector<float> out(220, 1.0f), in(220, -1.0f);
        for (std::size_t pos = 0; pos < out.size(); pos += static_cast<std::size_t>(block)) {
            const int n = static_cast<int>(
                std::min<std::size_t>(static_cast<std::size_t>(block), out.size() - pos));
            float* op[] = {out.data() + pos};
            const float* ip[] = {in.data() + pos};
            xf.mix(op, ip, 1, n);
        }
        REQUIRE(xf.finished());
        for (std::int64_t p = 0; p < 220; ++p) {
            float o = 0.0f, i = 0.0f;
            processing_switch_gains_at(plan, p, o, i);
            REQUIRE_THAT(out[static_cast<std::size_t>(p)], WithinAbs(o - i, 1e-6));
        }
    }
}

TEST_CASE("processing switch: no dropout and no step where a cut has both",
          "[crossfade][processing-switch]") {
    // Into a slower realisation (64 -> 10240 samples of latency) and out of
    // one. A cut is the negative control: the measurement must see its
    // dropout (a fresh delay line emits its latency of silence) and its step.
    for (int block : {64, 256, 1024}) {
        for (auto latencies : {std::pair<std::size_t, std::size_t>{64, 10240},
                               std::pair<std::size_t, std::size_t>{10240, 64}}) {
            const auto cut = run_switch(latencies.first, latencies.second, false, block);
            const auto faded = run_switch(latencies.first, latencies.second, true, block);
            INFO("block " << block << ", latency " << latencies.first << " -> " << latencies.second
                          << "; cut: step " << cut.worst_step << " silent " << cut.silent_run
                          << "; crossfade: step " << faded.worst_step << " silent "
                          << faded.silent_run << "; steady step " << faded.steady_step);
            // Control: the cut is caught, by its dropout or by its step.
            REQUIRE(
                (cut.silent_run >= latencies.second - 1 || cut.worst_step > 2.0 * cut.steady_step));
            // The switch: never silent, and no step. Equal power across two
            // phases of one sine is a sine whose amplitude swells by at most
            // sqrt(2) mid-fade, so its steepest slope may too -- a cut's
            // step is an order of magnitude beyond that.
            REQUIRE(faded.silent_run < 4);
            REQUIRE(faded.worst_step <= 1.5 * faded.steady_step);
        }
    }
}
