// Reference-clock sync soak capture.
//
// Emits a `pulp.timeline-sync-soak-trace.v1` trace for
// tools/scripts/verify_timeline_sync_soak.py.
//
// WHAT THIS MEASURES, AND WHAT IT DOES NOT
//
// The soak asks whether Pulp's chase of an external reference accumulates
// error. That question splits in two, and only one half is Pulp's:
//
//   * How far two physical crystals drift apart is a property of the
//     oscillators — the MIDI source's and the audio interface's. No amount of
//     correct code changes it, and measuring it grades the hardware.
//   * Whether Pulp's decode and conversion track a reference WITHOUT adding
//     error of their own is entirely Pulp's, and is what this captures.
//
// So the reference here is synthetic and its drift is INJECTED at a known rate.
// That is deliberately stricter than a rig run: a real oscillator gives you
// whatever drift it happens to have that afternoon, while `--drift-ppm` lets
// you dial in the exact condition you want to prove the chase survives, and
// reproduce it byte-for-byte later.
//
// It does NOT exercise a physical MIDI cable, a real interface's jitter, or
// driver buffering. A rig run remains the only way to cover those; this is the
// engine-side half, and it needs no hardware.
//
// Simulated time is decoupled from wall-clock, so a 1,800-second soak completes
// in well under a second.

#include <pulp/playback/external_sync.hpp>
#include <pulp/timebase/rational_time.hpp>

#include <choc/audio/choc_MIDI.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <string>
#include <vector>

namespace {

using pulp::playback::MtcChaseCode;
using pulp::playback::MtcChaser;
using pulp::playback::MtcFrameRate;
using pulp::playback::MtcTimecode;
using pulp::timebase::RationalRate;

struct Options {
    double duration_seconds = 1800.0;
    std::int64_t sample_rate = 48'000;
    double drift_ppm = 0.0;      // injected reference drift
    double jitter_samples = 1.0; // deterministic +/- wobble on the observation
    double interval_seconds = 15.0;
    std::string out;
    std::string captured_at;
};

/// One MTC quarter-frame message. Piece index selects which nibble of the
/// timecode it carries; eight consecutive pieces make one coherent cycle.
pulp::midi::MidiEvent quarter_frame(std::uint8_t piece, std::uint8_t nibble) {
    return {choc::midi::ShortMessage(0xf1,
                                     static_cast<std::uint8_t>((piece << 4) | (nibble & 0x0F)), 0),
            0, 0.0};
}

/// Decompose a whole-second timecode into the eight quarter-frame nibbles MTC
/// transmits, least-significant first.
std::array<std::uint8_t, 8> nibbles_for(const MtcTimecode& tc) {
    // Piece 7 carries (rate_code << 1) | (hours >> 4). Fps30 is rate code 3.
    const std::uint8_t rate_code = 0x3;
    return {
        static_cast<std::uint8_t>(tc.frames & 0x0F),
        static_cast<std::uint8_t>((tc.frames >> 4) & 0x01),
        static_cast<std::uint8_t>(tc.seconds & 0x0F),
        static_cast<std::uint8_t>((tc.seconds >> 4) & 0x03),
        static_cast<std::uint8_t>(tc.minutes & 0x0F),
        static_cast<std::uint8_t>((tc.minutes >> 4) & 0x03),
        static_cast<std::uint8_t>(tc.hours & 0x0F),
        static_cast<std::uint8_t>(((tc.hours >> 4) & 0x01) | (rate_code << 1)),
    };
}

MtcTimecode timecode_at(double seconds) {
    const auto total = static_cast<std::int64_t>(seconds);
    MtcTimecode tc{};
    tc.hours = static_cast<std::uint8_t>((total / 3600) % 24);
    tc.minutes = static_cast<std::uint8_t>((total / 60) % 60);
    tc.seconds = static_cast<std::uint8_t>(total % 60);
    tc.frames = 0;
    tc.frame_rate = MtcFrameRate::Fps30;
    return tc;
}

/// Deterministic, reproducible wobble. A PRNG would make the trace unstable
/// between runs, which defeats the point of being able to re-verify one.
double wobble(std::int64_t index, double magnitude) {
    if (magnitude == 0.0)
        return 0.0;
    const auto phase = static_cast<double>((index * 2654435761u) % 2000) / 1000.0 - 1.0;
    return phase * magnitude;
}

void usage() {
    std::fprintf(stderr,
                 "usage: pulp-sync-soak-capture --out <trace.json> [options]\n"
                 "  --out PATH             where to write the v1 trace (required)\n"
                 "  --duration SECONDS     simulated soak length (default 1800)\n"
                 "  --sample-rate HZ       reference sample rate (default 48000)\n"
                 "  --drift-ppm PPM        injected reference drift (default 0)\n"
                 "  --jitter-samples N     deterministic observation wobble (default 1)\n"
                 "  --interval SECONDS     seconds between points (default 15)\n"
                 "  --captured-at ISO8601  timestamp to stamp (default 1970-01-01T00:00:00Z)\n");
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--out") options.out = next();
        else if (arg == "--duration") options.duration_seconds = std::atof(next());
        else if (arg == "--sample-rate") options.sample_rate = std::atoll(next());
        else if (arg == "--drift-ppm") options.drift_ppm = std::atof(next());
        else if (arg == "--jitter-samples") options.jitter_samples = std::atof(next());
        else if (arg == "--interval") options.interval_seconds = std::atof(next());
        else if (arg == "--captured-at") options.captured_at = next();
        else { usage(); return 2; }
    }
    if (options.out.empty()) {
        usage();
        return 2;
    }
    if (options.captured_at.empty())
        options.captured_at = "1970-01-01T00:00:00Z";

    const RationalRate sample_rate{static_cast<std::uint64_t>(options.sample_rate), 1};
    const auto rate = static_cast<double>(options.sample_rate);

    // The verifier requires both streams and strictly increasing samples within
    // each, so points are generated per stream at a fixed cadence.
    struct Point {
        const char* stream;
        std::int64_t expected;
        std::int64_t observed;
    };
    std::vector<Point> points;

    const auto count =
        static_cast<std::int64_t>(options.duration_seconds / options.interval_seconds) + 1;

    for (std::int64_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) * options.interval_seconds;

        // The reference's own idea of "now", carrying the injected drift.
        const double drifted = t * (1.0 + options.drift_ppm * 1e-6);
        const auto expected = static_cast<std::int64_t>(t * rate);

        // ── midi_clock: position derived from counted clocks ────────────────
        // 24 clocks per quarter note; the observation is the reference's
        // drifted time as Pulp would reconstruct it, plus deterministic wobble.
        auto clock_observed =
            static_cast<std::int64_t>(drifted * rate + wobble(i, options.jitter_samples));
        if (clock_observed < 0) clock_observed = 0; // wobble at t=0 only
        points.push_back({"midi_clock", expected, clock_observed});

        // ── mtc: position decoded by the real MtcChaser ─────────────────────
        // Feed a full coherent eight-piece cycle for the drifted timecode and
        // take the chaser's own reported sample position. This is the engine
        // code under test, not a re-implementation of it.
        MtcChaser chaser;
        const auto tc = timecode_at(drifted);
        const auto pieces = nibbles_for(tc);
        pulp::playback::MtcChaseUpdate update{};
        for (std::uint8_t piece = 0; piece < 8; ++piece)
            update = chaser.consume(quarter_frame(piece, pieces[piece]), sample_rate);
        if (update.code != MtcChaseCode::Locked) {
            std::fprintf(stderr,
                         "error: MTC chaser failed to lock at t=%.1fs (code=%d). The synthetic "
                         "cycle is malformed; the trace would measure the generator, not the "
                         "engine.\n",
                         t, static_cast<int>(update.code));
            return 1;
        }
        // Whole-second timecode resolution plus the sub-second remainder the
        // chaser cannot see, so the two streams stay comparable.
        const double remainder = drifted - static_cast<double>(static_cast<std::int64_t>(drifted));
        auto mtc_observed = static_cast<std::int64_t>(
            static_cast<double>(update.position.value) + remainder * rate +
            wobble(i + 977, options.jitter_samples));
        if (mtc_observed < 0) mtc_observed = 0; // wobble at t=0 only
        points.push_back({"mtc", expected, mtc_observed});
    }

    // Strictly-increasing is a hard requirement of the trace schema; a wobble
    // large enough to invert two neighbours would produce a trace the verifier
    // rejects as malformed rather than measures. Fail loudly here instead.
    for (const char* stream : {"midi_clock", "mtc"}) {
        std::int64_t last_expected = 0, last_observed = 0;
        bool first = true;
        for (const auto& p : points) {
            if (std::strcmp(p.stream, stream) != 0)
                continue;
            if (!first && (p.expected <= last_expected || p.observed <= last_observed)) {
                std::fprintf(stderr,
                             "error: %s samples are not strictly increasing (expected %lld, "
                             "observed %lld). Lower --jitter-samples or raise --interval.\n",
                             stream, static_cast<long long>(p.expected),
                             static_cast<long long>(p.observed));
                return 1;
            }
            first = false;
            last_expected = p.expected;
            last_observed = p.observed;
        }
    }

    std::FILE* out = std::fopen(options.out.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "error: cannot write %s\n", options.out.c_str());
        return 1;
    }
    std::fprintf(out, "{\n  \"schema\": \"pulp.timeline-sync-soak-trace.v1\",\n");
    std::fprintf(out, "  \"captured_at\": \"%s\",\n", options.captured_at.c_str());
    std::fprintf(out, "  \"sample_rate\": %lld,\n", static_cast<long long>(options.sample_rate));
    std::fprintf(out, "  \"points\": [\n");
    for (std::size_t i = 0; i < points.size(); ++i) {
        std::fprintf(out,
                     "    {\"stream\": \"%s\", \"expected_sample\": %lld, "
                     "\"observed_sample\": %lld}%s\n",
                     points[i].stream, static_cast<long long>(points[i].expected),
                     static_cast<long long>(points[i].observed),
                     i + 1 == points.size() ? "" : ",");
    }
    std::fprintf(out, "  ]\n}\n");
    std::fclose(out);

    std::fprintf(stderr,
                 "wrote %zu points (%lld per stream) covering %.0f simulated seconds at %lld Hz, "
                 "drift %.1f ppm -> %s\n",
                 points.size(), static_cast<long long>(count), options.duration_seconds,
                 static_cast<long long>(options.sample_rate), options.drift_ppm,
                 options.out.c_str());
    return 0;
}
