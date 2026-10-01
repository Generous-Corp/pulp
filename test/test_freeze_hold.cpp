// FreezeHold direct-API tests: capture-window freshness and age, engage
// progress, transport-jump history clearing, time-based sizing and runtime
// hold length, the public held-state pipeline, phase precision over long
// holds, stereo instantaneous-frequency estimation, and snapshot /
// serialize / restore.
//
// Frame-level tests drive FreezeHold with synthetic single-bin tones so the
// assertions are exact; the audio-level test runs the hold as the pre-mask
// stage of a SpectralMaskProcessor over real, non-stationary audio and
// judges the held content by its spectral peak.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/fft.hpp>
#include <pulp/signal/freeze_hold.hpp>
#include <pulp/signal/rt_work_counter.hpp>
#include <pulp/signal/spectral_mask_processor.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

extern std::atomic<long> g_alloc_count; // sentinel in test_spectral_frame_engine.cpp

namespace {

constexpr double kPi = 3.14159265358979323846;

double wrap(double p) { return std::remainder(p, 2.0 * kPi); }

// One analysis frame group holding a single-bin tone. The tone sits in
// `bin`; its phase advances per hop at the frequency of `bin + offset_bins`,
// so offset 0 is a bin-centred tone. Channel c carries the tone rotated by
// c * channel_phase radians.
class ToneFrames {
public:
    ToneFrames(int fft_size, int channels, int hop)
        : fft_(fft_size), hop_(hop), bins_(fft_size / 2 + 1),
          storage_(static_cast<size_t>(channels),
                   std::vector<std::complex<float>>(static_cast<size_t>(bins_))),
          ptrs_(static_cast<size_t>(channels)) {
        for (size_t ch = 0; ch < storage_.size(); ++ch) ptrs_[ch] = storage_[ch].data();
    }

    std::complex<float>* const* tone(int bin, long index, double channel_phase = 0.0,
                                     double offset_bins = 0.0) {
        const double omega = 2.0 * kPi * (bin + offset_bins) / fft_;
        for (size_t ch = 0; ch < storage_.size(); ++ch) {
            std::fill(storage_[ch].begin(), storage_[ch].end(), std::complex<float>{});
            const double phase = omega * hop_ * static_cast<double>(index)
                               + channel_phase * static_cast<double>(ch);
            storage_[ch][static_cast<size_t>(bin)] =
                std::polar(1.0f, static_cast<float>(wrap(phase)));
        }
        return ptrs_.data();
    }

    std::complex<float>* const* frames() { return ptrs_.data(); }
    const std::vector<std::complex<float>>& channel(int ch) const {
        return storage_[static_cast<size_t>(ch)];
    }
    int bins() const { return bins_; }

private:
    int fft_;
    int hop_;
    int bins_;
    std::vector<std::vector<std::complex<float>>> storage_;
    std::vector<std::complex<float>*> ptrs_;
};

FreezeHold::Config small_config(int channels = 1) {
    FreezeHold::Config config;
    config.fft_size = 1024;
    config.channels = channels;
    config.analysis_hop = 256;
    config.capture_frames = 8;
    config.crossfade_frames = 6;
    return config;
}

bool pure_hold(const FreezeHold& hold) {
    return hold.is_engaged() && !hold.is_releasing() && hold.engage_progress() == 1.0f;
}

int peak_bin(const std::vector<std::complex<float>>& frame) {
    int best = 1;
    for (int k = 1; k < static_cast<int>(frame.size()); ++k)
        if (std::abs(frame[static_cast<size_t>(k)]) > std::abs(frame[static_cast<size_t>(best)]))
            best = k;
    return best;
}

// Bins with non-zero held magnitude (channel 0).
std::vector<int> held_bins(const FreezeHold& hold) {
    std::vector<int> out;
    const auto mags = hold.held_magnitudes(0);
    for (size_t k = 0; k < mags.size(); ++k)
        if (mags[k] != 0.0f) out.push_back(static_cast<int>(k));
    return out;
}

std::vector<int> bin_range(int first, int last) {
    std::vector<int> out;
    for (int b = first; b <= last; ++b) out.push_back(b);
    return out;
}

double peak_hz(const std::vector<float>& x, int start, int n, double sample_rate) {
    Fft fft(n);
    std::vector<std::complex<float>> buf(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float w = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / n));
        buf[static_cast<size_t>(i)] = {x[static_cast<size_t>(start + i)] * w, 0.0f};
    }
    fft.forward(buf.data());
    int kmax = 1;
    for (int k = 1; k < n / 2; ++k)
        if (std::abs(buf[static_cast<size_t>(k)]) > std::abs(buf[static_cast<size_t>(kmax)]))
            kmax = k;
    return kmax * sample_rate / n;
}

double window_rms_db(const std::vector<float>& x, int start, int length) {
    double acc = 0.0;
    for (int i = start; i < start + length; ++i)
        acc += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return 10.0 * std::log10(acc / length + 1e-300);
}

// Adapts a FreezeHold to the mask processor's pre-mask stage and toggles it
// on exact analysis-frame indices so the schedule is block-size independent.
struct ScheduledFreezeStage final : SpectralPreMaskStage {
    FreezeHold hold;
    long frame = 0;
    std::vector<std::pair<long, bool>> schedule; // (frame index, frozen)

    void process_frames(std::complex<float>* const* frames, int channels,
                        int num_bins) noexcept override {
        for (const auto& [at, frozen] : schedule)
            if (at == frame) hold.set_frozen(frozen);
        hold.process_group(frames, channels, num_bins);
        ++frame;
    }
};

} // namespace

TEST_CASE("FreezeHold re-freeze after release holds only post-release frames",
          "[signal][freeze]") {
    // Tone A before the first freeze, tone B from partway through the first
    // hold onward. A quick re-freeze after the release must hold B alone: the
    // capture window restarts at the release instead of keeping the frames
    // captured before the previous freeze. A steady input (one tone
    // throughout) cannot tell the two apart.
    constexpr int kToneA = 40;
    constexpr int kToneB = 120;
    const auto config = small_config();
    ToneFrames tones(config.fft_size, 1, config.analysis_hop);

    // 57 is the first frame after the six-frame release fade that started at
    // 50 has completed; 52 re-requests the freeze while that fade still runs
    // (the release is committed, so the old hold does not come back).
    for (const long refreeze_at : {57L, 52L}) {
        DYNAMIC_SECTION("re-freeze at frame " << refreeze_at) {
            FreezeHold hold;
            hold.prepare(config);
            int first_hold_peak = -1;
            int second_hold_peak = -1;
            float second_hold_a = -1.0f;
            float second_hold_b = -1.0f;
            float quietest_peak = 1.0f;
            for (long f = 0; f < 110; ++f) {
                if (f == 20) hold.set_frozen(true);
                if (f == 50) hold.set_frozen(false);
                if (f == refreeze_at) hold.set_frozen(true);
                const int bin = f < 30 ? kToneA : kToneB;
                hold.process_group(tones.tone(bin, f), 1, tones.bins());
                const auto& out = tones.channel(0);
                if (f == 45) first_hold_peak = peak_bin(out);
                if (f == 100) {
                    second_hold_peak = peak_bin(out);
                    second_hold_a = std::abs(out[kToneA]);
                    second_hold_b = std::abs(out[kToneB]);
                }
                // Never muted: the loudest bin never collapses (re-arming
                // passes live frames through).
                quietest_peak = std::min(quietest_peak,
                                         std::abs(out[static_cast<size_t>(peak_bin(out))]));
            }
            // Control: the same probe reports A while the first hold plays,
            // although the input is already B.
            REQUIRE(first_hold_peak == kToneA);
            REQUIRE(pure_hold(hold));
            INFO("second hold |A| " << second_hold_a << ", |B| " << second_hold_b);
            REQUIRE(second_hold_peak == kToneB);
            REQUIRE(second_hold_a == 0.0f);
            REQUIRE_THAT(second_hold_b, WithinAbs(1.0, 1e-5));
            REQUIRE(quietest_peak > 0.5f);
        }
    }
}

TEST_CASE("FreezeHold averages exactly the last capture window of frames",
          "[signal][freeze]") {
    // Frame f carries a tone in its own bin (10 + f), so the set of non-zero
    // held bins names exactly which frames a hold averaged: the hold never
    // contains audio older than the last `capture_frames` analysis frames.
    // Running sums must name exactly the same frames as the latch-time sum.
    auto config = small_config();
    config.average = GENERATE(FreezeHoldAverage::at_latch, FreezeHoldAverage::running);
    CAPTURE(static_cast<int>(config.average));
    ToneFrames tones(config.fft_size, 1, config.analysis_hop);
    auto bin_of = [](long f) { return 10 + static_cast<int>(f); };
    FreezeHold hold;
    hold.prepare(config);

    SECTION("first freeze, then quick re-freezes after a release") {
        long f = 0;
        for (; f <= 20; ++f) {
            if (f == 20) hold.set_frozen(true);
            hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
        }
        REQUIRE(held_bins(hold) == bin_range(bin_of(13), bin_of(20)));
        REQUIRE_THAT(hold.held_magnitudes(0)[static_cast<size_t>(bin_of(13))],
                     WithinAbs(1.0 / 8.0, 1e-6));

        for (; f < 40; ++f) hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
        hold.set_frozen(false); // release at 40, fade completes at 46
        for (; f < 47; ++f) hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
        hold.set_frozen(true);
        hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins()); // frame 47
        REQUIRE(hold.is_latched());
        REQUIRE(held_bins(hold) == bin_range(bin_of(40), bin_of(47)));

        // A shorter hold length applies to the next latch only.
        hold.set_capture_frames(3);
        REQUIRE(held_bins(hold) == bin_range(bin_of(40), bin_of(47)));
        for (++f; f < 60; ++f) hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
        hold.set_frozen(false); // release at 60
        for (; f < 63; ++f) hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
        hold.set_frozen(true); // during the fade: latches once it completes
        for (; f < 66; ++f) {
            hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
            REQUIRE(hold.is_releasing());
            REQUIRE(held_bins(hold) == bin_range(bin_of(40), bin_of(47)));
        }
        // Frame 66 completes the release and latches the three newest frames.
        hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
        REQUIRE(hold.is_latched());
        REQUIRE_FALSE(hold.is_releasing());
        REQUIRE(held_bins(hold) == bin_range(bin_of(64), bin_of(66)));
    }

    SECTION("a history clear while armed re-fills from later frames") {
        hold.set_frozen(true); // armed from the first frame
        long f = 0;
        for (; f < 5; ++f) hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
        hold.clear_history();
        for (; f < 12; ++f) {
            hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins());
            REQUIRE_FALSE(hold.is_latched());
            // Armed, not muted: live passes through untouched.
            REQUIRE(std::abs(tones.channel(0)[static_cast<size_t>(bin_of(f))]) == 1.0f);
        }
        hold.process_group(tones.tone(bin_of(f), f), 1, tones.bins()); // frame 12
        REQUIRE(hold.is_latched());
        REQUIRE(held_bins(hold) == bin_range(bin_of(5), bin_of(12)));
    }
}

TEST_CASE("FreezeHold re-freeze through a mask stage holds post-release audio",
          "[signal][freeze][spectral-mask-processor]") {
    // Audio-level version: real STFT frames of a non-stationary signal (A,
    // then B from partway through the first hold), identity mask, hold as
    // the pre-mask stage. Judged by the rendered output's spectral peak.
    constexpr double kSr = 48000.0;
    constexpr double kToneA = 750.0;
    constexpr double kToneB = 3000.0;
    constexpr int kFft = 1024;
    constexpr int kHop = 256;

    SpectralMaskProcessorConfig config;
    config.frame.fft_size = kFft;
    config.frame.analysis_hop = kHop;
    config.frame.channels = 1;
    config.frame.max_block = 512;
    config.frame.window = WindowFunction::Type::hann;
    config.sample_rate = static_cast<float>(kSr);
    SpectralMaskProcessor processor;
    REQUIRE(processor.prepare(config));

    ScheduledFreezeStage stage;
    FreezeHold::Config hold_config;
    hold_config.fft_size = kFft;
    hold_config.channels = 1;
    hold_config.analysis_hop = kHop;
    stage.hold.prepare(hold_config);
    // Freeze at 20, release at 50, quick re-freeze at 57 (fade done at 56).
    stage.schedule = {{20, true}, {50, false}, {57, true}};
    processor.set_pre_mask_stage(&stage);

    // Frame f completes at input sample kFft + f * kHop; switch the input to
    // B at frame 30 so every frame from 34 on is pure B.
    constexpr int kSwitch = kFft + 30 * kHop;
    constexpr int kLength = 36864;
    std::vector<float> input(kLength), output(kLength);
    for (int i = 0; i < kLength; ++i) {
        const double f = i < kSwitch ? kToneA : kToneB;
        input[static_cast<size_t>(i)] =
            static_cast<float>(0.5 * std::sin(2.0 * kPi * f * i / kSr));
    }
    for (int pos = 0; pos < kLength; pos += 480) {
        const int n = std::min(480, kLength - pos);
        const float* in[] = {input.data() + pos};
        float* out[] = {output.data() + pos};
        REQUIRE(processor.process(in, out, n));
    }

    const int latency = processor.latency_samples();
    auto frame_out = [&](long frame) { return kFft + static_cast<int>(frame) * kHop + latency; };
    // Control: the first hold (pure hold over frames 26..49) still plays A
    // over B input.
    const double first = peak_hz(output, frame_out(30), 2048, kSr);
    // The second hold is pure from frame 63.
    const double second = peak_hz(output, frame_out(64), 2048, kSr);
    INFO("first hold " << first << " Hz, second hold " << second << " Hz");
    REQUIRE_THAT(first, WithinAbs(kToneA, 2.0 * kSr / 2048));
    REQUIRE_THAT(second, WithinAbs(kToneB, 2.0 * kSr / 2048));
    // Never muted across both holds, the release, and the re-freeze.
    for (int s = frame_out(15); s + 480 < frame_out(100); s += 480) {
        INFO("window at " << s);
        REQUIRE(window_rms_db(output, s, 480) > -20.0);
    }
}

TEST_CASE("FreezeHold engage progress tracks the spectral crossfade exactly",
          "[signal][freeze]") {
    auto config = small_config();
    ToneFrames tones(config.fft_size, 1, config.analysis_hop);
    ToneFrames other(config.fft_size, 1, config.analysis_hop);
    for (const int crossfade : {1, 6, 12}) {
        DYNAMIC_SECTION("crossfade_frames " << crossfade) {
            config.crossfade_frames = crossfade;
            // Two holds fed identical history, then different live input:
            // their outputs agree exactly iff the live frame contributes
            // nothing, i.e. iff the crossfade is at pure hold.
            FreezeHold a, b;
            a.prepare(config);
            b.prepare(config);
            long f = 0;
            auto step_both = [&](int live_a, int live_b) {
                a.process_group(tones.tone(live_a, f), 1, tones.bins());
                b.process_group(other.tone(live_b, f), 1, other.bins());
                ++f;
                return tones.channel(0) == other.channel(0);
            };
            for (int i = 0; i < 8; ++i) step_both(30, 30);
            REQUIRE(a.engage_progress() == 0.0f);

            a.set_frozen(true);
            b.set_frozen(true);
            for (int k = 1; k <= crossfade; ++k) {
                // The latch frame is captured into the window, so it must match
                // for the two holds to latch the same content.
                const bool identical = k == 1 ? step_both(30, 30) : step_both(30, 90);
                INFO("engage frame " << k);
                REQUIRE(a.is_latched());
                REQUIRE(a.engage_progress()
                        == static_cast<float>(k) / static_cast<float>(crossfade));
                REQUIRE(pure_hold(a) == (k == crossfade));
                if (k > 1) REQUIRE(identical == pure_hold(a));
            }
            for (int i = 0; i < 4; ++i) {
                REQUIRE(step_both(30, 90));
                REQUIRE(pure_hold(a));
            }

            a.set_frozen(false);
            b.set_frozen(false);
            REQUIRE(a.is_releasing());
            REQUIRE_FALSE(pure_hold(a));
            for (int k = crossfade - 1; k >= 0; --k) {
                step_both(30, 90);
                INFO("release step " << k);
                REQUIRE(a.is_latched());
                REQUIRE(a.engage_progress()
                        == static_cast<float>(k) / static_cast<float>(crossfade));
            }
            // The next frame completes the release: live passes unchanged.
            step_both(30, 90);
            REQUIRE_FALSE(a.is_latched());
            REQUIRE_FALSE(a.is_releasing());
            REQUIRE(a.engage_progress() == 0.0f);
            REQUIRE(std::abs(tones.channel(0)[30]) == 1.0f);
        }
    }
}

TEST_CASE("FreezeHold keeps phase coherence and the stereo offset through a long hold",
          "[signal][freeze]") {
    // A high bin advances by thousands of radians per frame. Held phases
    // must stay wrapped, or converting them to float at mix time quantizes
    // them coarsely enough to scramble the inter-channel offset and the
    // frame-to-frame advance within seconds.
    FreezeHold::Config config;
    config.fft_size = 8192;
    config.channels = 2;
    config.analysis_hop = 2048;
    config.capture_frames = 8;
    config.crossfade_frames = 6;
    config.synthesis = GENERATE(FreezeHoldSynthesis::polar, FreezeHoldSynthesis::rotor);
    CAPTURE(static_cast<int>(config.synthesis));
    FreezeHold hold;
    hold.prepare(config);
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    constexpr int kBin = 1700; // ~10 kHz at 48 kHz
    constexpr double kOffset = 0.7;
    const double advance = 2.0 * kPi * kBin / config.fft_size * config.analysis_hop;
    // One frame's random-walk bound plus float rounding of a unit phasor.
    const double walk = static_cast<double>(config.phase_jitter) + 1e-4;

    long f = 0;
    for (; f < 8; ++f) hold.process_group(tones.tone(kBin, f, kOffset), 2, tones.bins());
    hold.set_frozen(true);
    double worst_offset = 0.0;
    double worst_coherence = 0.0;
    double previous = 0.0;
    bool have_previous = false;
    for (; f < 2400; ++f) {
        hold.process_group(tones.tone(kBin, f, kOffset), 2, tones.bins());
        if (!pure_hold(hold)) continue;
        const auto l = tones.channel(0)[kBin];
        const auto r = tones.channel(1)[kBin];
        worst_offset = std::max(worst_offset,
                                std::abs(wrap(std::arg(r * std::conj(l)) - kOffset)));
        const double phase = std::arg(l);
        if (have_previous)
            worst_coherence = std::max(worst_coherence,
                                       std::abs(wrap(phase - previous - advance)));
        previous = phase;
        have_previous = true;
    }
    INFO("worst inter-channel error " << worst_offset << " rad, worst per-frame "
                                      << "advance error " << worst_coherence << " rad");
    REQUIRE(worst_offset < 1e-3);
    REQUIRE(worst_coherence < walk);
}

TEST_CASE("FreezeHold estimates the frequency of an anti-phase stereo bin",
          "[signal][freeze]") {
    // L = -R: the channel SUM cancels, so a frequency estimate taken from
    // the sum's phase is noise. The hold must still advance the bin at the
    // tone's true (off-centre) frequency.
    auto config = small_config(2);
    config.phase_jitter = 0.0f;
    config.frequency = GENERATE(FreezeHoldFrequency::newest,
                                FreezeHoldFrequency::energy_weighted);
    CAPTURE(static_cast<int>(config.frequency));
    FreezeHold hold;
    hold.prepare(config);
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    constexpr int kBin = 60;
    constexpr double kOffsetBins = 0.3;
    const double omega = 2.0 * kPi * (kBin + kOffsetBins) / config.fft_size;

    long f = 0;
    for (; f < 8; ++f) hold.process_group(tones.tone(kBin, f, kPi, kOffsetBins), 2, tones.bins());
    hold.set_frozen(true);
    hold.process_group(tones.tone(kBin, f, kPi, kOffsetBins), 2, tones.bins());
    REQUIRE(hold.is_latched());
    const double estimate = hold.instantaneous_frequency()[kBin];
    INFO("estimated " << estimate << " rad/sample, true " << omega);
    REQUIRE_THAT(estimate, WithinAbs(omega, 1e-6));
    // Control: a correlated (in-phase) pair gives the same answer.
    FreezeHold in_phase;
    in_phase.prepare(config);
    for (long g = 0; g <= 8; ++g) {
        if (g == 8) in_phase.set_frozen(true);
        in_phase.process_group(tones.tone(kBin, g, 0.0, kOffsetBins), 2, tones.bins());
    }
    REQUIRE_THAT(in_phase.instantaneous_frequency()[kBin], WithinAbs(omega, 1e-6));
}

TEST_CASE("FreezeHold clear_history keeps a playing hold; reset drops it",
          "[signal][freeze]") {
    const auto config = small_config(2);
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    ToneFrames tones_ref(config.fft_size, 2, config.analysis_hop);
    FreezeHold hold;
    hold.prepare(config);
    long f = 0;
    for (; f < 8; ++f) hold.process_group(tones.tone(50, f, 0.3), 2, tones.bins());
    hold.set_frozen(true);
    for (; f < 20; ++f) hold.process_group(tones.tone(50, f, 0.3), 2, tones.bins());
    REQUIRE(pure_hold(hold));

    FreezeHold reference = hold;
    hold.clear_history(); // a transport jump
    for (long g = 0; g < 30; ++g, ++f) {
        hold.process_group(tones.tone(200, f), 2, tones.bins());
        reference.process_group(tones_ref.tone(200, f), 2, tones_ref.bins());
        INFO("frame " << g);
        REQUIRE(pure_hold(hold));
        REQUIRE(tones.channel(0) == tones_ref.channel(0));
        REQUIRE(tones.channel(1) == tones_ref.channel(1));
    }

    hold.reset();
    REQUIRE_FALSE(hold.is_latched());
    REQUIRE_FALSE(hold.is_engaged());
    REQUIRE_FALSE(hold.has_hold());
    hold.process_group(tones.tone(200, f), 2, tones.bins());
    REQUIRE(std::abs(tones.channel(0)[200]) == 1.0f); // live passes
    REQUIRE(std::abs(tones.channel(0)[50]) == 0.0f);
}

TEST_CASE("FreezeHold seconds-based sizing keeps the reference timing at any geometry",
          "[signal][freeze]") {
    using Timing = FreezeHoldReferenceTiming;
    SECTION("frame-count configs are unchanged") {
        FreezeHold::Config frames;
        frames.fft_size = 4096;
        frames.analysis_hop = 512;
        const auto resolved = FreezeHold::resolve(frames);
        REQUIRE(resolved.capture_frames == 8);
        REQUIRE(resolved.crossfade_frames == 6);
        REQUIRE(resolved.phase_jitter == 0.015f);
        REQUIRE(resolved.max_capture_frames == 8);
    }
    SECTION("the reference geometry resolves to today's frame counts") {
        // RealtimePitchTimeProcessor's quality mode: 4096/512 at 48 kHz.
        FreezeHold::Config timed;
        timed.fft_size = 4096;
        timed.analysis_hop = 512;
        timed.sample_rate = 48000.0;
        const auto resolved = FreezeHold::resolve(timed);
        REQUIRE(resolved.capture_frames == 8);
        REQUIRE(resolved.crossfade_frames == 6);
        REQUIRE_THAT(resolved.phase_jitter, WithinAbs(0.015, 1e-9));
    }
    SECTION("8192/2048 keeps the durations within one hop") {
        FreezeHold::Config timed;
        timed.fft_size = 8192;
        timed.analysis_hop = 2048;
        timed.sample_rate = 48000.0;
        const auto resolved = FreezeHold::resolve(timed);
        const double hop_seconds = 2048.0 / 48000.0;
        REQUIRE_THAT(resolved.capture_frames * hop_seconds,
                     WithinAbs(Timing::kCaptureSeconds, hop_seconds));
        REQUIRE_THAT(resolved.crossfade_frames * hop_seconds,
                     WithinAbs(Timing::kCrossfadeSeconds, hop_seconds));
        // The walk's spread per sqrt(second) is the reference one.
        REQUIRE_THAT(resolved.phase_jitter * std::sqrt(48000.0 / 2048.0),
                     WithinAbs(Timing::kPhaseJitterPerSqrtSecond, 1e-6));
    }
}

TEST_CASE("FreezeHold hold length is settable at runtime within the prepared bound",
          "[signal][freeze]") {
    FreezeHold::Config timed;
    timed.fft_size = 1024;
    timed.analysis_hop = 256;
    timed.sample_rate = 48000.0;
    timed.max_capture_seconds = 2.0;
    FreezeHold hold;
    hold.prepare(timed);
    REQUIRE(hold.config().max_capture_frames == 375); // 2 s of 256-sample hops
    REQUIRE(hold.capture_frames() == hold.config().capture_frames);

    hold.set_capture_seconds(1.0);
    REQUIRE(hold.capture_frames() == 188);
    hold.set_capture_seconds(10.0);
    REQUIRE(hold.capture_frames() == 375);
    hold.set_capture_frames(1);
    REQUIRE(hold.capture_frames() == 2);
    hold.set_capture_seconds(0.5);
    hold.reset();
    REQUIRE(hold.capture_frames() == 94); // a control, kept across reset

    // The longer window really is averaged: arming waits for 94 frames.
    ToneFrames tones(1024, 1, 256);
    hold.set_frozen(true);
    for (long f = 0; f < 93; ++f) {
        hold.process_group(tones.tone(40, f), 1, tones.bins());
        REQUIRE_FALSE(hold.is_latched());
    }
    hold.process_group(tones.tone(40, 93), 1, tones.bins());
    REQUIRE(hold.is_latched());

    // A frame-count config ignores the seconds form.
    FreezeHold frames;
    frames.prepare(small_config());
    frames.set_capture_seconds(1.0);
    REQUIRE(frames.capture_frames() == 8);
}

TEST_CASE("FreezeHold exposes its held state as a renderable pipeline",
          "[signal][freeze]") {
    const auto config = small_config(2);
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    ToneFrames rendered(config.fft_size, 2, config.analysis_hop);
    FreezeHold hold;
    hold.prepare(config);
    REQUIRE_FALSE(hold.write_hold(rendered.frames(), 2, rendered.bins()));

    long f = 0;
    for (; f < 8; ++f) hold.process_group(tones.tone(50, f, 0.4), 2, tones.bins());
    hold.set_frozen(true);
    for (; f < 20; ++f) hold.process_group(tones.tone(50, f, 0.4), 2, tones.bins());
    REQUIRE(pure_hold(hold));

    // write_hold renders exactly the frame process_group would emit next.
    FreezeHold driven = hold;
    REQUIRE(hold.write_hold(rendered.frames(), 2, rendered.bins()));
    driven.process_group(tones.tone(300, f), 2, tones.bins());
    REQUIRE(rendered.channel(0) == tones.channel(0));
    REQUIRE(rendered.channel(1) == tones.channel(1));
    REQUIRE_FALSE(hold.write_hold(rendered.frames(), 1, rendered.bins())); // wrong shape

    // advance_hold follows process_group's phase advance, PRNG included.
    hold.advance_hold(1);
    for (int ch = 0; ch < 2; ++ch) {
        const auto a = hold.held_phases(ch);
        const auto b = driven.held_phases(ch);
        REQUIRE(std::equal(a.begin(), a.end(), b.begin()));
    }
    driven.process_group(tones.tone(300, f + 1), 2, tones.bins());
    REQUIRE(hold.write_hold(rendered.frames(), 2, rendered.bins()));
    REQUIRE(rendered.channel(0) == tones.channel(0));

    // Without a random walk, rewinding undoes advancing.
    auto still = config;
    still.phase_jitter = 0.0f;
    FreezeHold plain;
    plain.prepare(still);
    for (long g = 0; g <= 8; ++g) {
        if (g == 8) plain.set_frozen(true);
        plain.process_group(tones.tone(50, g, 0.4, 0.2), 2, tones.bins());
    }
    const auto before = std::vector<double>(plain.held_phases(1).begin(),
                                            plain.held_phases(1).end());
    plain.advance_hold(7);
    plain.rewind_hold_phases(7);
    const auto after = plain.held_phases(1);
    double worst = 0.0;
    for (size_t k = 0; k < before.size(); ++k)
        worst = std::max(worst, std::abs(wrap(after[k] - before[k])));
    REQUIRE(worst < 1e-9);
}

TEST_CASE("FreezeHold snapshot restores the same held output",
          "[signal][freeze][state]") {
    const auto config = small_config(2);
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    ToneFrames tones_copy(config.fft_size, 2, config.analysis_hop);
    FreezeHold source;
    source.prepare(config);

    FreezeHoldSnapshot image;
    REQUIRE(image.prepare(config.fft_size, config.channels, config.analysis_hop));
    REQUIRE_FALSE(source.snapshot(image)); // nothing latched yet

    long f = 0;
    for (; f < 8; ++f) source.process_group(tones.tone(50, f, 0.4), 2, tones.bins());
    source.set_frozen(true);
    for (; f < 40; ++f) source.process_group(tones.tone(50, f, 0.4), 2, tones.bins());
    REQUIRE(pure_hold(source));
    REQUIRE(source.snapshot(image));
    REQUIRE(image.valid());
    REQUIRE(image.capture_frames == config.capture_frames);
    REQUIRE(image.engaged);
    REQUIRE(image.fade_step == config.crossfade_frames);

    std::vector<std::uint8_t> bytes;
    REQUIRE(image.write_bytes(bytes));
    REQUIRE(bytes.size() == FreezeHoldSnapshot::serialized_size(config.fft_size, config.channels));
    FreezeHoldSnapshot decoded;
    REQUIRE(FreezeHoldSnapshot::read_bytes(bytes.data(), bytes.size(), decoded));

    const struct {
        FreezeRestoreEngage engage;
        int settle;
    } cases[] = {{FreezeRestoreEngage::immediate, 1},
                 {FreezeRestoreEngage::as_captured, 1},
                 {FreezeRestoreEngage::crossfade, config.crossfade_frames}};
    for (const auto& c : cases) {
        DYNAMIC_SECTION("engage " << static_cast<int>(c.engage)) {
            FreezeHold restored;
            restored.prepare(config);
            // Stage from another thread, as a host state recall would.
            bool staged = false;
            std::thread producer([&] { staged = restored.stage_restore(decoded, c.engage); });
            producer.join();
            REQUIRE(staged);
            REQUIRE(restored.restore_pending());

            // Continue the source hold and the restored hold on different
            // live input: once the restored hold is at pure hold, every
            // frame is bit-identical to the source's.
            FreezeHold reference = source;
            for (long g = 0; g < 60; ++g) {
                reference.process_group(tones.tone(50, f + g, 0.4), 2, tones.bins());
                restored.process_group(tones_copy.tone(200, f + g, 0.1), 2,
                                       tones_copy.bins());
                INFO("frame " << g);
                REQUIRE_FALSE(restored.restore_pending());
                REQUIRE(restored.is_engaged());
                if (g + 1 >= c.settle) {
                    REQUIRE(pure_hold(restored));
                    REQUIRE(tones.channel(0) == tones_copy.channel(0));
                    REQUIRE(tones.channel(1) == tones_copy.channel(1));
                } else {
                    REQUIRE_FALSE(pure_hold(restored));
                }
            }
        }
    }
}

TEST_CASE("FreezeHold as-captured restore resumes a release in progress",
          "[signal][freeze][state]") {
    const auto config = small_config(1);
    ToneFrames tones(config.fft_size, 1, config.analysis_hop);
    FreezeHold source;
    source.prepare(config);
    long f = 0;
    for (; f < 8; ++f) source.process_group(tones.tone(50, f), 1, tones.bins());
    source.set_frozen(true);
    for (; f < 20; ++f) source.process_group(tones.tone(50, f), 1, tones.bins());
    source.set_frozen(false);
    for (int i = 0; i < 2; ++i, ++f) source.process_group(tones.tone(50, f), 1, tones.bins());
    REQUIRE(source.is_releasing());

    FreezeHoldSnapshot image;
    REQUIRE(image.prepare(config.fft_size, 1, config.analysis_hop));
    REQUIRE(source.snapshot(image));
    REQUIRE(image.releasing);
    REQUIRE_FALSE(image.engaged);
    REQUIRE(image.fade_step == 4);

    FreezeHold restored;
    restored.prepare(config);
    REQUIRE(restored.stage_restore(image, FreezeRestoreEngage::as_captured));
    restored.process_group(tones.tone(50, f), 1, tones.bins());
    REQUIRE(restored.is_releasing());
    REQUIRE_FALSE(restored.is_engaged());
    REQUIRE(restored.engage_progress() == 3.0f / 6.0f);

    // A hold whose release already completed restores as kept-but-silent.
    for (int i = 0; i < 6; ++i, ++f) source.process_group(tones.tone(50, f), 1, tones.bins());
    REQUIRE_FALSE(source.is_latched());
    REQUIRE(source.snapshot(image));
    REQUIRE_FALSE(image.engaged);
    REQUIRE_FALSE(image.releasing);
    FreezeHold idle;
    idle.prepare(config);
    REQUIRE(idle.stage_restore(image, FreezeRestoreEngage::as_captured));
    idle.process_group(tones.tone(300, 0), 1, tones.bins());
    REQUIRE(idle.has_hold());
    REQUIRE_FALSE(idle.is_latched());
    REQUIRE(std::abs(tones.channel(0)[300]) == 1.0f); // live passes
    REQUIRE(std::abs(tones.channel(0)[50]) == 0.0f);
}

TEST_CASE("FreezeHold snapshot images reject malformed or mismatched input",
          "[signal][freeze][state]") {
    const auto config = small_config(2);
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    FreezeHold source;
    source.prepare(config);
    for (long f = 0; f < 8; ++f) source.process_group(tones.tone(50, f), 2, tones.bins());
    source.set_frozen(true);
    source.process_group(tones.tone(50, 8), 2, tones.bins());
    FreezeHoldSnapshot image;
    REQUIRE(image.prepare(config.fft_size, config.channels, config.analysis_hop));
    REQUIRE(source.snapshot(image));
    std::vector<std::uint8_t> bytes;
    REQUIRE(image.write_bytes(bytes));

    FreezeHoldSnapshot out;
    // Control: the unmodified image parses.
    REQUIRE(FreezeHoldSnapshot::read_bytes(bytes.data(), bytes.size(), out));

    auto rejects = [&](std::vector<std::uint8_t> mutated) {
        FreezeHoldSnapshot sink;
        return !FreezeHoldSnapshot::read_bytes(mutated.data(), mutated.size(), sink);
    };
    auto put_u32 = [](std::vector<std::uint8_t>& m, size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) m[at + static_cast<size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    SECTION("bad magic") { auto m = bytes; m[0] = 'X'; REQUIRE(rejects(m)); }
    SECTION("future version") { auto m = bytes; put_u32(m, 4, 2); REQUIRE(rejects(m)); }
    SECTION("truncated") { auto m = bytes; m.pop_back(); REQUIRE(rejects(m)); }
    SECTION("trailing bytes") { auto m = bytes; m.push_back(0); REQUIRE(rejects(m)); }
    SECTION("fft size out of bounds") { auto m = bytes; put_u32(m, 8, 65536); REQUIRE(rejects(m)); }
    SECTION("channel count out of bounds") { auto m = bytes; put_u32(m, 12, 200); REQUIRE(rejects(m)); }
    SECTION("fade step past the crossfade") { auto m = bytes; put_u32(m, 28, 99); REQUIRE(rejects(m)); }
    SECTION("unknown flag bits") { auto m = bytes; put_u32(m, 32, 4); REQUIRE(rejects(m)); }
    SECTION("reserved field set") { auto m = bytes; put_u32(m, 36, 1); REQUIRE(rejects(m)); }
    SECTION("non-finite magnitude") {
        auto m = bytes;
        const float nan = std::nanf("");
        std::uint32_t bits = 0;
        std::memcpy(&bits, &nan, sizeof(bits));
        put_u32(m, FreezeHoldSnapshot::kHeaderBytes, bits);
        REQUIRE(rejects(m));
    }
    SECTION("zero PRNG state") {
        auto m = bytes;
        for (size_t i = 48; i < 56; ++i) m[i] = 0;
        REQUIRE(rejects(m));
    }
    SECTION("null data") { REQUIRE_FALSE(FreezeHoldSnapshot::read_bytes(nullptr, 0, out)); }

    SECTION("restore refuses a different geometry or sample rate") {
        FreezeHold other;
        other.prepare(small_config(1));
        REQUIRE_FALSE(other.stage_restore(out));
        auto hop = small_config(2);
        hop.analysis_hop = 128;
        other.prepare(hop);
        REQUIRE_FALSE(other.stage_restore(out));

        auto at_48k = small_config(2);
        at_48k.sample_rate = 48000.0;
        auto at_44k = at_48k;
        at_44k.sample_rate = 44100.0;
        FreezeHold timed;
        timed.prepare(at_48k);
        FreezeHold timed_source;
        timed_source.prepare(at_44k);
        for (long f = 0; f <= timed_source.capture_frames(); ++f) {
            if (f == timed_source.capture_frames()) timed_source.set_frozen(true);
            timed_source.process_group(tones.tone(50, f), 2, tones.bins());
        }
        FreezeHoldSnapshot rated;
        REQUIRE(rated.prepare(1024, 2, 256));
        REQUIRE(timed_source.snapshot(rated));
        REQUIRE(rated.sample_rate == 44100.0);
        REQUIRE_FALSE(timed.stage_restore(rated));
        REQUIRE(other.restore_pending() == false);
        // An image with no recorded rate is accepted by a timed hold.
        REQUIRE(timed.stage_restore(out));
    }
    SECTION("a second restore waits for the first to be adopted") {
        FreezeHold target;
        target.prepare(config);
        REQUIRE(target.stage_restore(out));
        REQUIRE_FALSE(target.stage_restore(out));
        target.reset(); // a staged recall survives a host reset
        REQUIRE(target.restore_pending());
        target.process_group(tones.tone(50, 0), 2, tones.bins());
        REQUIRE_FALSE(target.restore_pending());
        REQUIRE(target.stage_restore(out));
        target.prepare(config); // re-prepare drops it
        REQUIRE_FALSE(target.restore_pending());
    }
    SECTION("snapshot refuses mismatched storage") {
        FreezeHoldSnapshot small;
        REQUIRE(small.prepare(512, 2, 128));
        REQUIRE_FALSE(source.snapshot(small));
    }
}

TEST_CASE("FreezeHold state, pipeline, and hold-length calls allocate nothing",
          "[signal][freeze][rt-safety]") {
    auto config = small_config(2);
    config.max_capture_frames = 32;
    if (GENERATE(false, true)) {
        // Every opt-in cost mode, staged engage included.
        config.synthesis = FreezeHoldSynthesis::rotor;
        config.frequency = FreezeHoldFrequency::energy_weighted;
        config.average = FreezeHoldAverage::running;
        config.engage_bins_per_hop = 64;
    }
    CAPTURE(static_cast<int>(config.synthesis));
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    FreezeHold hold;
    hold.prepare(config);
    FreezeHoldSnapshot image;
    REQUIRE(image.prepare(config.fft_size, config.channels, config.analysis_hop));
    for (long f = 0; f < 8; ++f) hold.process_group(tones.tone(50, f), 2, tones.bins());
    hold.set_frozen(true);
    hold.process_group(tones.tone(50, 8), 2, tones.bins());
    // A staged engage refuses a snapshot until its rotors are built.
    for (long f = 8; hold.engage_pending(); ++f)
        hold.process_group(tones.tone(50, f), 2, tones.bins());

    const long before = g_alloc_count.load();
    bool ok = hold.snapshot(image);
    hold.set_capture_frames(20);
    hold.clear_history();
    hold.set_frozen(false);
    for (long f = 9; f < 20; ++f) hold.process_group(tones.tone(50, f), 2, tones.bins());
    ok = hold.stage_restore(image, FreezeRestoreEngage::immediate) && ok;
    hold.process_group(tones.tone(50, 20), 2, tones.bins());
    ok = hold.write_hold(tones.frames(), 2, tones.bins()) && ok;
    hold.advance_hold(3);
    hold.rewind_hold_phases(3);
    const bool engaged = pure_hold(hold);
    REQUIRE(g_alloc_count.load() == before);
    REQUIRE(ok);
    REQUIRE(engaged);
}

// ── per-callback cost ──────────────────────────────────────────────────────
//
// A host drops the buffer whose callback overruns, and an offline render
// cannot see that. These tests bound the work of individual frame groups with
// the deterministic rt::RtWorkCounter counts (FFT / transcendental / per-bin
// accumulation), never with wall time. Each pairs the bounded configuration
// with the default one as its control: the default must show the burst, or
// the count is not seeing the work.

namespace {

// Frames with energy in every bin, so the cost does not depend on content.
class BroadbandFrames {
public:
    BroadbandFrames(int fft_size, int channels)
        : bins_(fft_size / 2 + 1),
          storage_(static_cast<size_t>(channels),
                   std::vector<std::complex<float>>(static_cast<size_t>(bins_))),
          ptrs_(static_cast<size_t>(channels)) {
        for (size_t ch = 0; ch < storage_.size(); ++ch) ptrs_[ch] = storage_[ch].data();
    }

    std::complex<float>* const* frame(long index) {
        for (size_t ch = 0; ch < storage_.size(); ++ch)
            for (int k = 0; k < bins_; ++k)
                storage_[ch][static_cast<size_t>(k)] = std::polar(
                    0.5f + 0.25f * static_cast<float>((k + index) % 3),
                    static_cast<float>(wrap(0.37 * k * (index + 1) + 0.9 * static_cast<double>(ch))));
        return ptrs_.data();
    }
    int bins() const { return bins_; }

private:
    int bins_;
    std::vector<std::vector<std::complex<float>>> storage_;
    std::vector<std::complex<float>*> ptrs_;
};

FreezeHold::Config large_config() {
    FreezeHold::Config config;
    config.fft_size = 8192;
    config.channels = 2;
    config.analysis_hop = 256;
    config.capture_frames = 188;  // a 2 s hold at a 512-sample hop and 48 kHz
    config.max_capture_frames = 188;
    config.crossfade_frames = 6;
    return config;
}

struct LatchCost {
    pulp::signal::rt::RtWorkCounts steady; // the capturing frame group before the latch
    pulp::signal::rt::RtWorkCounts latch;  // the latching frame group
};

LatchCost measure_latch(FreezeHold::Config config, int capture_frames) {
    config.capture_frames = capture_frames;
    FreezeHold hold;
    hold.prepare(config);
    BroadbandFrames frames(config.fft_size, config.channels);
    const int channels = config.channels;
    long f = 0;
    // Fill the whole ring so the steady frame group slides a full window.
    for (; f < config.max_capture_frames + 4; ++f)
        hold.process_group(frames.frame(f), channels, frames.bins());
    LatchCost cost;
    {
        pulp::signal::rt::RtWorkCounter counter;
        hold.process_group(frames.frame(f++), channels, frames.bins());
        cost.steady = counter.delta();
    }
    hold.set_frozen(true);
    {
        pulp::signal::rt::RtWorkCounter counter;
        hold.process_group(frames.frame(f++), channels, frames.bins());
        cost.latch = counter.delta();
    }
    REQUIRE(hold.is_latched());
    return cost;
}

} // namespace

TEST_CASE("FreezeHold running average makes latch cost independent of capture length",
          "[signal][freeze][rt-cost]") {
    REQUIRE(pulp::signal::rt::kWorkCountersEnabled);
    const auto config = large_config();
    const auto channel_bins = static_cast<std::uint64_t>(config.channels)
                            * static_cast<std::uint64_t>(config.fft_size / 2 + 1);

    SECTION("control: the latch-time average scales with the window") {
        const auto short_hold = measure_latch(config, 2);
        const auto long_hold = measure_latch(config, 188);
        INFO("latch bins: 2 frames " << short_hold.latch.bins << ", 188 frames "
                                     << long_hold.latch.bins);
        CHECK(short_hold.latch.bins == 2 * channel_bins);
        CHECK(long_hold.latch.bins == 188 * channel_bins);
    }

    SECTION("running sums: the latch only divides") {
        auto running = config;
        running.average = FreezeHoldAverage::running;
        const auto short_hold = measure_latch(running, 2);
        const auto long_hold = measure_latch(running, 188);
        INFO("latch bins: 2 frames " << short_hold.latch.bins << ", 188 frames "
                                     << long_hold.latch.bins);
        CHECK(short_hold.latch.bins == long_hold.latch.bins);
        // Capture slides one frame in and one out; the latch adds one divide
        // per channel bin on top.
        CHECK(long_hold.steady.bins == 2 * channel_bins);
        CHECK(long_hold.latch.bins == long_hold.steady.bins + channel_bins);
    }
}

TEST_CASE("FreezeHold rotor synthesis performs no transcendental per held hop",
          "[signal][freeze][rt-cost]") {
    REQUIRE(pulp::signal::rt::kWorkCountersEnabled);
    auto config = large_config();
    config.capture_frames = 8;
    const auto channel_bins = static_cast<std::uint64_t>(config.channels)
                            * static_cast<std::uint64_t>(config.fft_size / 2 + 1);
    const auto held_trig = [&](FreezeHoldSynthesis synthesis) {
        auto c = config;
        c.synthesis = synthesis;
        FreezeHold hold;
        hold.prepare(c);
        BroadbandFrames frames(c.fft_size, c.channels);
        long f = 0;
        for (; f < 200; ++f) {
            if (f == 190) hold.set_frozen(true);
            hold.process_group(frames.frame(f), c.channels, frames.bins());
        }
        REQUIRE(pure_hold(hold));
        pulp::signal::rt::RtWorkCounter counter;
        for (int i = 0; i < 100; ++i, ++f)
            hold.process_group(frames.frame(f), c.channels, frames.bins());
        return counter.delta().trig;
    };
    // Control: the original path evaluates one polar per channel bin per hop.
    CHECK(held_trig(FreezeHoldSynthesis::polar) == 100 * channel_bins);
    CHECK(held_trig(FreezeHoldSynthesis::rotor) == 0);
}

TEST_CASE("FreezeHold staged engage spreads rotor construction across frame groups",
          "[signal][freeze][rt-cost]") {
    REQUIRE(pulp::signal::rt::kWorkCountersEnabled);
    auto config = large_config();
    config.capture_frames = 8;
    config.synthesis = FreezeHoldSynthesis::rotor;
    config.frequency = FreezeHoldFrequency::energy_weighted;
    config.average = FreezeHoldAverage::running;
    const int bins = config.fft_size / 2 + 1;

    const auto run = [&](int bins_per_hop, std::vector<std::uint64_t>& per_hop) {
        auto c = config;
        c.engage_bins_per_hop = bins_per_hop;
        FreezeHold hold;
        hold.prepare(c);
        BroadbandFrames frames(c.fft_size, c.channels);
        long f = 0;
        for (; f < 40; ++f) hold.process_group(frames.frame(f), c.channels, frames.bins());
        {
            // Steady capture does no transcendental in this configuration.
            pulp::signal::rt::RtWorkCounter counter;
            hold.process_group(frames.frame(f++), c.channels, frames.bins());
            CHECK(counter.delta().trig == 0);
        }
        hold.set_frozen(true);
        do {
            pulp::signal::rt::RtWorkCounter counter;
            hold.process_group(frames.frame(f++), c.channels, frames.bins());
            per_hop.push_back(counter.delta().trig);
        } while (hold.engage_pending());
        REQUIRE(hold.is_latched());
        // Once staging completes, held hops are transcendental-free again.
        pulp::signal::rt::RtWorkCounter counter;
        hold.process_group(frames.frame(f++), c.channels, frames.bins());
        CHECK(counter.delta().trig == 0);
    };

    SECTION("control: an unstaged latch builds every rotor in one frame group") {
        std::vector<std::uint64_t> per_hop;
        run(0, per_hop);
        REQUIRE(per_hop.size() == 1);
        // One atan2 (frequency) and one polar (rotor) per bin.
        CHECK(per_hop[0] == 2u * static_cast<std::uint64_t>(bins));
    }

    SECTION("staged: at most engage_bins_per_hop bins per frame group") {
        constexpr int kChunk = 512;
        std::vector<std::uint64_t> per_hop;
        run(kChunk, per_hop);
        CHECK(per_hop.size() == static_cast<size_t>((bins + kChunk - 1) / kChunk));
        std::uint64_t total = 0;
        for (const auto t : per_hop) {
            CHECK(t <= 2u * kChunk);
            total += t;
        }
        CHECK(total == 2u * static_cast<std::uint64_t>(bins));
    }
}

TEST_CASE("FreezeHold rotor phasors keep unit modulus and their offsets over 1e6 hops",
          "[signal][freeze]") {
    FreezeHold::Config config;
    config.fft_size = 256;
    config.channels = 2;
    config.analysis_hop = 64;
    config.capture_frames = 2;
    config.phase_jitter = 0.0f;
    config.synthesis = FreezeHoldSynthesis::rotor;
    FreezeHold hold;
    hold.prepare(config);
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    constexpr int kBin = 37;
    constexpr double kOffset = 1.1;
    constexpr double kOffsetBins = 0.23;
    for (long f = 0; f < 3; ++f) {
        if (f == 2) hold.set_frozen(true);
        hold.process_group(tones.tone(kBin, f, kOffset, kOffsetBins), 2, tones.bins());
    }
    REQUIRE(hold.is_latched());
    const double start = hold.held_phases(0)[kBin];
    const double omega = hold.instantaneous_frequency()[kBin];
    REQUIRE_THAT(omega, WithinAbs(2.0 * kPi * (kBin + kOffsetBins) / config.fft_size, 1e-7));

    constexpr long kHops = 1000000;
    hold.advance_hold(static_cast<int>(kHops));

    // Modulus: the rendered frame carries the held magnitude to -120 dB.
    ToneFrames rendered(config.fft_size, 2, config.analysis_hop);
    REQUIRE(hold.write_hold(rendered.frames(), 2, rendered.bins()));
    const double mag = hold.held_magnitudes(0)[kBin];
    for (int ch = 0; ch < 2; ++ch) {
        const double modulus = std::abs(rendered.channel(ch)[kBin]) / mag;
        INFO("channel " << ch << " modulus " << modulus);
        CHECK(std::abs(modulus - 1.0) < 1e-6);
    }
    // Inter-channel offset and accumulated rotation.
    const double offset = wrap(hold.held_phases(1)[kBin] - hold.held_phases(0)[kBin]);
    CHECK_THAT(offset, WithinAbs(kOffset, 1e-6));
    const double expected = wrap(start + std::fmod(omega * config.analysis_hop * kHops, 2.0 * kPi));
    CHECK(std::abs(wrap(hold.held_phases(0)[kBin] - expected)) < 1e-6);
}

TEST_CASE("FreezeHold energy-weighted frequency follows the content that dominates a bin",
          "[signal][freeze]") {
    // A bin loud at one frequency for most of the window and faint at another
    // in the newest frames — the leakage of a later sound. The newest-frame
    // estimate takes the faint content's frequency; the energy-weighted one
    // keeps the dominant content's.
    auto config = small_config(1);
    config.capture_frames = 32;
    config.phase_jitter = 0.0f;
    constexpr int kBin = 60;
    constexpr double kLoud = 0.3;   // bin offset of the dominant content
    constexpr double kFaint = -0.4; // bin offset of the newest, faint content
    const double hop = config.analysis_hop;
    const auto omega = [&](double offset) {
        return 2.0 * kPi * (kBin + offset) / config.fft_size;
    };
    const auto estimate = [&](FreezeHoldFrequency frequency) {
        auto c = config;
        c.frequency = frequency;
        FreezeHold hold;
        hold.prepare(c);
        std::vector<std::complex<float>> frame(static_cast<size_t>(c.fft_size / 2 + 1));
        std::complex<float>* ptrs[] = {frame.data()};
        double phase = 0.0;
        for (long f = 0; f < 32; ++f) {
            const bool faint = f >= 26;
            phase = wrap(phase + omega(faint ? kFaint : kLoud) * hop);
            std::fill(frame.begin(), frame.end(), std::complex<float>{});
            frame[kBin] = std::polar(faint ? 0.01f : 1.0f, static_cast<float>(phase));
            if (f == 31) hold.set_frozen(true);
            hold.process_group(ptrs, 1, static_cast<int>(frame.size()));
        }
        REQUIRE(hold.is_latched());
        return hold.instantaneous_frequency()[kBin];
    };
    const double newest = estimate(FreezeHoldFrequency::newest);
    const double weighted = estimate(FreezeHoldFrequency::energy_weighted);
    INFO("newest " << newest << ", weighted " << weighted << ", dominant " << omega(kLoud)
                   << ", faint " << omega(kFaint));
    // Control: the default estimate really is the faint, newest content's.
    CHECK_THAT(newest, WithinAbs(omega(kFaint), 1e-6));
    // Within a hundredth of a bin of the dominant content.
    CHECK_THAT(weighted, WithinAbs(omega(kLoud), 0.01 * 2.0 * kPi / config.fft_size));
}

TEST_CASE("FreezeHold long hold across a change keeps the older sound audible",
          "[signal][freeze][spectral-mask-processor]") {
    // Chord A for 3 s, chord B after; freeze 0.4 s after the change with a
    // 2 s hold, so most of the window is chord A. With the newest-frame
    // frequency, chord A's bins rotate at chord B's leakage frequencies and
    // their frames cancel: chord A is barely heard. With the energy-weighted
    // frequency it plays at its own pitches and near its captured level.
    constexpr double kSr = 48000.0;
    constexpr int kFft = 8192;
    constexpr int kHop = 512;
    constexpr double kChordA[] = {261.63, 329.63, 392.00, 523.25};
    constexpr double kChordB[] = {369.99, 466.16, 554.37, 739.99};
    constexpr double kAmp = 0.12;
    constexpr double kChange = 3.0;
    constexpr double kFreezeAt = kChange + 0.4;
    constexpr int kLength = static_cast<int>((kFreezeAt + 2.0) * kSr);

    std::vector<float> input(static_cast<size_t>(kLength));
    for (int i = 0; i < kLength; ++i) {
        const double t = i / kSr;
        double v = 0.0;
        for (const double f : t < kChange ? kChordA : kChordB)
            v += kAmp * std::sin(2.0 * kPi * f * t);
        input[static_cast<size_t>(i)] = static_cast<float>(v);
    }

    // Least-squares amplitude of a known frequency over a window.
    const auto tone_amplitude = [&](const std::vector<float>& x, int start, int n, double hz) {
        double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
        for (int i = start; i < start + n; ++i) {
            const double s = std::sin(2.0 * kPi * hz * i / kSr);
            const double c = std::cos(2.0 * kPi * hz * i / kSr);
            const double v = x[static_cast<size_t>(i)];
            ss += s * s; cc += c * c; sc += s * c; xs += v * s; xc += v * c;
        }
        const double det = ss * cc - sc * sc;
        const double a = (xs * cc - xc * sc) / det;
        const double b = (xc * ss - xs * sc) / det;
        return std::hypot(a, b);
    };

    const auto render = [&](FreezeHoldFrequency frequency) {
        SpectralMaskProcessorConfig config;
        config.frame.fft_size = kFft;
        config.frame.analysis_hop = kHop;
        config.frame.channels = 1;
        config.frame.max_block = 512;
        config.frame.window = WindowFunction::Type::hann;
        config.sample_rate = static_cast<float>(kSr);
        SpectralMaskProcessor processor;
        REQUIRE(processor.prepare(config));
        ScheduledFreezeStage stage;
        FreezeHold::Config hold_config;
        hold_config.fft_size = kFft;
        hold_config.channels = 1;
        hold_config.analysis_hop = kHop;
        hold_config.sample_rate = kSr;
        hold_config.capture_seconds = 2.0;
        hold_config.frequency = frequency;
        stage.hold.prepare(hold_config);
        // Frame f completes at input sample kFft + f * kHop.
        const long freeze_frame = std::lround((kFreezeAt * kSr - kFft) / kHop);
        stage.schedule = {{freeze_frame, true}};
        processor.set_pre_mask_stage(&stage);
        std::vector<float> output(static_cast<size_t>(kLength));
        for (int pos = 0; pos < kLength; pos += 480) {
            const int n = std::min(480, kLength - pos);
            const float* in[] = {input.data() + pos};
            float* out[] = {output.data() + pos};
            REQUIRE(processor.process(in, out, n));
        }
        REQUIRE(stage.hold.is_latched());
        const int from = static_cast<int>((kFreezeAt + 0.5) * kSr) + processor.latency_samples();
        const int count = static_cast<int>(0.8 * kSr);
        REQUIRE(from + count <= kLength);
        double a = 0.0;
        for (const double f : kChordA) a += tone_amplitude(output, from, count, f);
        return a / (4.0 * kAmp);
    };

    const double newest = render(FreezeHoldFrequency::newest);
    const double weighted = render(FreezeHoldFrequency::energy_weighted);
    INFO("chord A heard at " << 20.0 * std::log10(newest + 1e-12) << " dB (newest) and "
                             << 20.0 * std::log10(weighted + 1e-12)
                             << " dB (energy-weighted) against the input");
    // Control: the default reproduces the defect.
    CHECK(newest < 0.25);
    CHECK(weighted > 0.25);
}

TEST_CASE("FreezeHold rotor hold survives a snapshot round trip",
          "[signal][freeze][state]") {
    auto config = small_config(2);
    config.synthesis = FreezeHoldSynthesis::rotor;
    config.engage_bins_per_hop = 128;
    ToneFrames tones(config.fft_size, 2, config.analysis_hop);
    FreezeHold source;
    source.prepare(config);
    FreezeHoldSnapshot image;
    REQUIRE(image.prepare(config.fft_size, config.channels, config.analysis_hop));

    long f = 0;
    for (; f < 8; ++f) source.process_group(tones.tone(50, f, 0.4), 2, tones.bins());
    source.set_frozen(true);
    source.process_group(tones.tone(50, f++, 0.4), 2, tones.bins());
    REQUIRE(source.engage_pending());
    REQUIRE_FALSE(source.snapshot(image)); // staging incomplete
    for (; f < 40; ++f) source.process_group(tones.tone(50, f, 0.4), 2, tones.bins());
    REQUIRE_FALSE(source.engage_pending());
    REQUIRE(source.snapshot(image));
    REQUIRE(image.valid());

    FreezeHold restored;
    restored.prepare(config);
    REQUIRE(restored.stage_restore(image, FreezeRestoreEngage::immediate));
    ToneFrames a(config.fft_size, 2, config.analysis_hop);
    ToneFrames b(config.fft_size, 2, config.analysis_hop);
    FreezeHold reference = source;
    for (long g = 0; g < 20; ++g) {
        reference.process_group(a.tone(50, f + g, 0.4), 2, a.bins());
        restored.process_group(b.tone(200, f + g, 0.1), 2, b.bins());
        if (restored.engage_pending()) continue;
        for (int ch = 0; ch < 2; ++ch) {
            INFO("frame " << g << " channel " << ch);
            CHECK(std::abs(a.channel(ch)[50] - b.channel(ch)[50]) < 1e-5f);
        }
    }
}

TEST_CASE("FreezeHold peak-lobe phase lock keeps short-term level stationary from the start",
          "[signal][freeze][spectral-mask-processor]") {
    // A pad whose partials come in slightly detuned pairs, so each partial's
    // main lobe holds two components and its bins carry different
    // increments, over a faint noise bed. Unlocked, the hold starts with the
    // latched frame's coherent lobes and each bin then drifts at its own
    // frequency: the level moves while the hold is already audible. Locked,
    // each lobe rotates at its peak's frequency and the other bins start at
    // random phases, so the first held frames already have the level the
    // hold settles to.
    constexpr double kSr = 48000.0;
    constexpr int kFft = 8192;
    constexpr int kHop = 512;
    constexpr double kNotes[] = {220.0, 277.18, 329.63, 440.0};
    constexpr double kDetune = 1.003;
    constexpr double kAmp = 0.08;
    constexpr double kFreezeAt = 1.0;
    constexpr int kLength = static_cast<int>((kFreezeAt + 3.0) * kSr);

    std::vector<float> input(static_cast<size_t>(kLength));
    std::uint64_t noise = 0x51a7u;
    for (int i = 0; i < kLength; ++i) {
        const double t = i / kSr;
        double v = 0.0;
        for (const double f : kNotes)
            v += kAmp * (std::sin(2.0 * kPi * f * t) + std::sin(2.0 * kPi * f * kDetune * t + 1.0));
        noise ^= noise << 13; noise ^= noise >> 7; noise ^= noise << 17;
        v += 0.004 * (static_cast<double>(noise >> 11) / 9007199254740992.0 * 2.0 - 1.0);
        input[static_cast<size_t>(i)] = static_cast<float>(v);
    }

    struct Profile {
        double early_db = 0.0;   // level of the first 0.5 s of pure hold, against settled
        double worst_db = 0.0;   // worst 50 ms window over the whole hold, against settled
        double settled_db = 0.0; // level of the last second
    };
    const auto render = [&](FreezeHoldPhaseLock lock) {
        SpectralMaskProcessorConfig config;
        config.frame.fft_size = kFft;
        config.frame.analysis_hop = kHop;
        config.frame.channels = 1;
        config.frame.max_block = 512;
        config.frame.window = WindowFunction::Type::hann;
        config.sample_rate = static_cast<float>(kSr);
        SpectralMaskProcessor processor;
        REQUIRE(processor.prepare(config));
        ScheduledFreezeStage stage;
        FreezeHold::Config hold_config;
        hold_config.fft_size = kFft;
        hold_config.channels = 1;
        hold_config.analysis_hop = kHop;
        hold_config.phase_lock = lock;
        stage.hold.prepare(hold_config);
        const long freeze_frame = std::lround((kFreezeAt * kSr - kFft) / kHop);
        stage.schedule = {{freeze_frame, true}};
        processor.set_pre_mask_stage(&stage);
        std::vector<float> output(static_cast<size_t>(kLength));
        for (int pos = 0; pos < kLength; pos += 480) {
            const int n = std::min(480, kLength - pos);
            const float* in[] = {input.data() + pos};
            float* out[] = {output.data() + pos};
            REQUIRE(processor.process(in, out, n));
        }
        REQUIRE(stage.hold.is_latched());
        // The first frame whose synthesis window is entirely pure hold.
        const int pure = kFft + static_cast<int>(freeze_frame + hold_config.crossfade_frames) * kHop
                       + processor.latency_samples();
        const int window = static_cast<int>(0.05 * kSr);
        Profile profile;
        const int settled_from = kLength - static_cast<int>(kSr);
        profile.settled_db = window_rms_db(output, settled_from, kLength - settled_from);
        profile.early_db = window_rms_db(output, pure, static_cast<int>(0.5 * kSr))
                         - profile.settled_db;
        for (int s = pure; s + window <= kLength; s += window / 2) {
            const double d = window_rms_db(output, s, window) - profile.settled_db;
            if (std::abs(d) > std::abs(profile.worst_db)) profile.worst_db = d;
        }
        return profile;
    };

    const auto unlocked = render(FreezeHoldPhaseLock::none);
    const auto locked = render(FreezeHoldPhaseLock::peak_lobe);
    INFO("first 0.5 s against the settled level: unlocked " << unlocked.early_db
         << " dB, locked " << locked.early_db << " dB; worst 50 ms window: unlocked "
         << unlocked.worst_db << " dB, locked " << locked.worst_db << " dB");
    // Control: without the lock the hold starts measurably above the level it
    // settles to (it decoheres while audible).
    CHECK(unlocked.early_db > 0.75);
    CHECK(std::abs(locked.early_db) < 0.25);
    // Short windows still beat (detuned pairs), but around a fixed level.
    CHECK(std::abs(locked.worst_db) < 1.0);
}
