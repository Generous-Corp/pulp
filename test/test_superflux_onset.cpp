// SuperFlux onset detection (Böck and Widmer, DAFx 2013): the streaming
// novelty front end, the offline detector, vibrato suppression, and the slice
// analyzer option.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/slice_point_analyzer.hpp>
#include <pulp/signal/analysis_frontends.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace pulp::audio;
using pulp::signal::StreamingAnalysisConfig;
using pulp::signal::SuperFluxConfig;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 44100.0;

struct Lcg {
    std::uint32_t state;
    double next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 8388608.0 - 1.0;
    }
};

BufferView<const float> view_of(const std::vector<float>& samples, const float** pointer) {
    *pointer = samples.data();
    return {pointer, 1, samples.size()};
}

struct DrumEvent {
    double time;
    int kind; // 0 kick, 1 snare, 2 hat
};

std::vector<DrumEvent> drum_pattern() {
    std::vector<DrumEvent> events;
    for (int beat = 0; beat < 7; ++beat) {
        const double t = 0.25 + 0.5 * beat;
        events.push_back({t, beat % 2 == 0 ? 0 : 1});
        events.push_back({t + 0.25, 2});
    }
    return events;
}

std::vector<float> render_drums(const std::vector<DrumEvent>& events, double seconds) {
    const int total = static_cast<int>(seconds * kFs);
    std::vector<double> mix(static_cast<std::size_t>(total), 0.0);
    Lcg rng{777};
    for (const auto& e : events) {
        const int start = static_cast<int>(e.time * kFs);
        for (int n = 0; start + n < total && n < static_cast<int>(0.4 * kFs); ++n) {
            const double t = n / kFs;
            double v = 0.0;
            if (e.kind == 0) // kick: falling sine
                v = 0.9 * std::sin(2.0 * kPi * (50.0 * t + 90.0 * 0.04 * (1.0 - std::exp(-t / 0.04)))) *
                    std::exp(-t / 0.12);
            else if (e.kind == 1) // snare: noise plus a body tone
                v = 0.6 * rng.next() * std::exp(-t / 0.06) +
                    0.4 * std::sin(2.0 * kPi * 190.0 * t) * std::exp(-t / 0.08);
            else // closed hat: short noise
                v = 0.35 * rng.next() * std::exp(-t / 0.015);
            mix[static_cast<std::size_t>(start + n)] += v;
        }
    }
    return {mix.begin(), mix.end()};
}

// A sustained harmonic tone with vibrato (depth in semitones, 5.5 Hz) and one
// noise hit at `hit_time`. The hit dominates the detector's normalization, as
// a real drum hit over a held note does.
std::vector<float> vibrato_with_hit(double depth_semitones, double hit_time, double seconds) {
    const int total = static_cast<int>(seconds * kFs);
    std::vector<float> out(static_cast<std::size_t>(total));
    Lcg rng{12345};
    double phase = 0.0;
    for (int n = 0; n < total; ++n) {
        const double t = n / kFs;
        const double env = std::min(1.0, t / 0.3);
        const double f = 330.0 * std::pow(2.0, depth_semitones * std::sin(2.0 * kPi * 5.5 * t) / 12.0);
        phase += 2.0 * kPi * f / kFs;
        double v = 0.0;
        for (int h = 1; h <= 10; ++h)
            v += std::sin(h * phase) / h;
        double sample = 0.25 * env * v;
        if (t >= hit_time)
            sample += 0.8 * rng.next() * std::exp(-(t - hit_time) / 0.03);
        out[static_cast<std::size_t>(n)] = static_cast<float>(sample);
    }
    return out;
}

int markers_away_from(const std::vector<OnsetMarker>& markers, double hit_time, double settle) {
    int count = 0;
    for (const auto& m : markers) {
        const double t = m.frame / kFs;
        if (t > settle && !(t > hit_time - 0.05 && t < hit_time + 0.1))
            ++count;
    }
    return count;
}

std::vector<float> tone_switch(double f1, double f2, double switch_time, double seconds) {
    const int total = static_cast<int>(seconds * kFs);
    std::vector<float> out(static_cast<std::size_t>(total));
    double phase = 0.0;
    for (int n = 0; n < total; ++n) {
        phase += 2.0 * kPi * (n / kFs < switch_time ? f1 : f2) / kFs;
        out[static_cast<std::size_t>(n)] = static_cast<float>(0.5 * std::sin(phase));
    }
    return out;
}

} // namespace

TEST_CASE("SuperFlux front end rejects invalid settings", "[audio][onset][superflux]") {
    pulp::signal::OnsetNoveltyFrontEndT<float, 2048, 1> front_end;
    StreamingAnalysisConfig config;
    config.sample_rate = kFs;
    config.channels = 1;
    config.fft_size = 2048;
    config.hop_size = 220;
    using Method = pulp::signal::OnsetDetectionMethod;
    CHECK(front_end.prepare(config, Method::SuperFlux));

    SuperFluxConfig bad;
    bad.max_filter_width = 4; // even
    CHECK_FALSE(front_end.prepare(config, Method::SuperFlux, bad));
    bad = {};
    bad.max_hz = bad.min_hz;
    CHECK_FALSE(front_end.prepare(config, Method::SuperFlux, bad));
    bad = {};
    bad.lag_frames = 9;
    CHECK_FALSE(front_end.prepare(config, Method::SuperFlux, bad));
    bad = {};
    bad.compression = 0.0;
    CHECK_FALSE(front_end.prepare(config, Method::SuperFlux, bad));

    auto not_power_of_two = config;
    not_power_of_two.fft_size = 1500;
    CHECK_FALSE(front_end.prepare(not_power_of_two, Method::SuperFlux));
}

TEST_CASE("SuperFlux novelty ignores a vibrato partial but not a new pitch",
          "[audio][onset][superflux][vibrato]") {
    const double f0 = 3000.0;
    SuperFluxConfig filtered, unfiltered;
    unfiltered.max_filter_width = 1;

    // Largest novelty among frames that start after the start-up transient.
    auto settled_peak = [](const std::vector<float>& x, const SuperFluxConfig& params) {
        pulp::signal::OnsetNoveltyFrontEndT<float, 2048, 1> front_end;
        StreamingAnalysisConfig config;
        config.sample_rate = kFs;
        config.channels = 1;
        config.fft_size = 2048;
        config.hop_size = 220;
        REQUIRE(front_end.prepare(config, pulp::signal::OnsetDetectionMethod::SuperFlux, params));
        const float* channel = x.data();
        double peak = 0.0;
        REQUIRE(front_end.process(&channel, 1, x.size(), [&](const auto& frame) noexcept {
            if (frame.window_start_frame > 20000)
                peak = std::max(peak, frame.novelty);
        }));
        return peak;
    };

    // Steady tone: nothing after the start-up.
    CHECK(settled_peak(tone_switch(f0, f0, 10.0, 1.5), filtered) < 1e-3);

    // A partial with half-semitone vibrato at constant level. Each frame it
    // moves about a band (24 per octave), so the unfiltered comparison keeps
    // seeing new energy; the maximum filter absorbs it.
    std::vector<float> vibrato(static_cast<std::size_t>(1.5 * kFs));
    double phase = 0.0;
    for (std::size_t n = 0; n < vibrato.size(); ++n) {
        const double f = f0 * std::exp2(0.5 * std::sin(2.0 * kPi * 5.5 * n / kFs) / 12.0);
        phase += 2.0 * kPi * f / kFs;
        vibrato[n] = static_cast<float>(0.5 * std::sin(phase));
    }
    const double with_filter = settled_peak(vibrato, filtered);
    const double without_filter = settled_peak(vibrato, unfiltered);
    INFO("with filter " << with_filter << " without " << without_filter);
    CHECK(without_filter > 3.0 * with_filter);

    // A new pitch six bands away is a new note either way, far above the
    // vibrato ripple.
    const auto new_note = tone_switch(f0, f0 * std::exp2(6.0 / 24.0), 0.8, 1.6);
    CHECK(settled_peak(new_note, filtered) > 5.0 * with_filter);
}

TEST_CASE("SuperFlux detects a synthetic drum pattern at the right frames",
          "[audio][onset][superflux]") {
    const auto events = drum_pattern();
    const auto audio = render_drums(events, 4.0);
    const float* pointer = nullptr;
    const auto result =
        OnsetDetector{}.detect(view_of(audio, &pointer), superflux_onset_config(kFs));
    REQUIRE(result.ok);

    // One marker per event, each within 25 ms of the true hit, nothing extra.
    CHECK(result.markers.size() == events.size());
    for (const auto& event : events) {
        const auto truth = static_cast<std::int64_t>(event.time * kFs);
        const auto tolerance = static_cast<std::int64_t>(0.025 * kFs);
        bool found = false;
        for (const auto& marker : result.markers) {
            if (std::llabs(static_cast<std::int64_t>(marker.frame) - truth) <= tolerance)
                found = true;
        }
        INFO("event at " << event.time << " kind " << event.kind);
        CHECK(found);
    }
    for (const auto& marker : result.markers)
        CHECK(marker.method == OnsetDetectionMethod::SuperFlux);
}

TEST_CASE("SuperFlux raises fewer false onsets than spectral flux on a vibrato tone",
          "[audio][onset][superflux][vibrato]") {
    const double hit = 1.5;
    for (const double depth : {0.3, 0.5, 1.0}) {
        const auto audio = vibrato_with_hit(depth, hit, 3.0);
        const float* pointer = nullptr;
        auto config = superflux_onset_config(kFs);
        config.min_confidence = 0.05;

        const auto super = OnsetDetector{}.detect(view_of(audio, &pointer), config);
        config.method = OnsetDetectionMethod::SpectralFlux;
        const auto plain = OnsetDetector{}.detect(view_of(audio, &pointer), config);
        REQUIRE(super.ok);
        REQUIRE(plain.ok);
        const int super_false = markers_away_from(super.markers, hit, 0.4);
        const int plain_false = markers_away_from(plain.markers, hit, 0.4);
        INFO("depth " << depth << " superflux false " << super_false << " spectral flux false "
                      << plain_false);
        // The real hit is still found.
        bool found = false;
        for (const auto& m : super.markers)
            found = found || (std::abs(m.frame / kFs - hit) < 0.05);
        CHECK(found);
        CHECK(super_false <= 1);
        CHECK(plain_false >= 8);
    }
}

TEST_CASE("SuperFlux maximum filter is what suppresses mid-depth vibrato",
          "[audio][onset][superflux][vibrato]") {
    const double hit = 1.5;
    const auto audio = vibrato_with_hit(0.5, hit, 3.0);
    const float* pointer = nullptr;
    auto config = superflux_onset_config(kFs);
    config.min_confidence = 0.05;
    const auto with_filter = OnsetDetector{}.detect(view_of(audio, &pointer), config);
    config.superflux.max_filter_width = 1;
    const auto without_filter = OnsetDetector{}.detect(view_of(audio, &pointer), config);
    REQUIRE(with_filter.ok);
    REQUIRE(without_filter.ok);
    CHECK(markers_away_from(with_filter.markers, hit, 0.4) <= 1);
    CHECK(markers_away_from(without_filter.markers, hit, 0.4) >= 5);
}

TEST_CASE("SuperFlux detector refuses settings it cannot honour", "[audio][onset][superflux]") {
    const std::vector<float> audio(200000, 0.1f);
    const float* pointer = nullptr;
    auto config = superflux_onset_config(kFs);
    config.sample_rate = 0.0;
    CHECK_FALSE(OnsetDetector{}.detect(view_of(audio, &pointer), config).ok);
    config = superflux_onset_config(kFs);
    config.frame_size = 3000; // not a power of two
    CHECK_FALSE(OnsetDetector{}.detect(view_of(audio, &pointer), config).ok);
    config = superflux_onset_config(kFs);
    config.frame_size = 131072; // beyond the fixed-capacity front end
    config.hop_size = 131072;
    CHECK_FALSE(OnsetDetector{}.detect(view_of(audio, &pointer), config).ok);
    config = superflux_onset_config(kFs);
    config.superflux.max_filter_width = 2;
    CHECK_FALSE(OnsetDetector{}.detect(view_of(audio, &pointer), config).ok);
}

TEST_CASE("SlicePointAnalyzer can detect with SuperFlux and slices at the hits",
          "[audio][onset][superflux][slice]") {
    const auto events = drum_pattern();
    const auto audio = render_drums(events, 4.0);
    const float* pointer = nullptr;
    const auto source = view_of(audio, &pointer);

    SlicePointAnalysisConfig config;
    config.source_sample_rate = kFs;
    config.snap_to_zero_crossing = false;
    config.min_slice_frames = static_cast<std::uint64_t>(0.05 * kFs);

    const SlicePointAnalyzer analyzer;
    const auto detected = analyzer.analyze_detected(source, superflux_onset_config(kFs), config);
    REQUIRE(detected.ok);

    // Same result as detecting first and passing the markers in.
    const auto markers = OnsetDetector{}.detect(source, superflux_onset_config(kFs)).markers;
    const auto manual = analyzer.analyze(source, markers, config);
    REQUIRE(manual.ok);
    CHECK(detected.map.markers.size() == manual.map.markers.size());
    CHECK(detected.map.markers.size() == events.size() + 1); // origin plus one per hit

    // Every hit has a slice start within 25 ms.
    for (const auto& event : events) {
        const auto truth = static_cast<std::int64_t>(event.time * kFs);
        bool found = false;
        for (const auto& marker : detected.map.markers)
            found = found || std::llabs(static_cast<std::int64_t>(marker.frame) - truth) <=
                                 static_cast<std::int64_t>(0.025 * kFs);
        CHECK(found);
    }

    // The analyzer's own sample rate overrides the detection config's.
    auto wrong = superflux_onset_config(kFs);
    wrong.sample_rate = 8000.0;
    const auto overridden = analyzer.analyze_detected(source, wrong, config);
    REQUIRE(overridden.ok);
    CHECK(overridden.map.markers.size() == detected.map.markers.size());

    // A rejected detection config yields a failed result.
    auto invalid = superflux_onset_config(kFs);
    invalid.superflux.max_filter_width = 2;
    CHECK_FALSE(analyzer.analyze_detected(source, invalid, config).ok);
    SlicePointAnalysisConfig bad_rate = config;
    bad_rate.source_sample_rate = 0.0;
    CHECK_FALSE(analyzer.analyze_detected(source, superflux_onset_config(kFs), bad_rate).ok);
}

TEST_CASE("SuperFlux streaming allocates nothing after prepare", "[audio][onset][superflux][rt-safety]") {
    pulp::signal::OnsetNoveltyFrontEndT<float, 2048, 1> front_end;
    StreamingAnalysisConfig config;
    config.sample_rate = kFs;
    config.channels = 1;
    config.fft_size = 2048;
    config.hop_size = 220;
    REQUIRE(front_end.prepare(config, pulp::signal::OnsetDetectionMethod::SuperFlux));
    const auto audio = render_drums(drum_pattern(), 2.0);
    const float* channel = audio.data();
    std::size_t frames = 0;
    double sum = 0.0;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(front_end.process(&channel, 1, audio.size(), [&](const auto& frame) noexcept {
            ++frames;
            sum += frame.novelty;
        }));
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(frames > 100);
    CHECK(sum > 0.0);

    // A non-finite sample is rejected and the analysis recovers.
    std::vector<float> poisoned = audio;
    poisoned[5000] = std::numeric_limits<float>::quiet_NaN();
    channel = poisoned.data();
    front_end.reset();
    CHECK_FALSE(front_end.process(&channel, 1, poisoned.size(), [](const auto&) noexcept {}));
}
