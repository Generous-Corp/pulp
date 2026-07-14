// Impulse-response loader coverage: the PCM (already-decoded) entry point and the
// shared post-decode pipeline behind it.
//
// read_impulse_response_pcm() exists because not every host can decode a file: a
// wasm build has no FormatRegistry, so the browser decodes with
// AudioContext.decodeAudioData and hands the raw planar float samples straight in.
// That makes it a decoder-adjacent surface fed by UNTRUSTED input (an ArrayBuffer
// a page just fetched), reached on a thread that must not OOM or spin: a hostile
// sample rate makes the Kaiser resampler demand a multi-GB tap design, and a NaN
// sample would spread through the convolver's FFT and silence the plugin. So the
// edge cases — not the happy path — are the contract here.
//
// The tests also drive detail::finish_impulse_response() directly. It is the tail
// BOTH entry points share (resample → drop group delay → clamp → reject non-finite
// → normalize), and its clamp at the TARGET rate only bites on resampler margin
// the callers cannot dial in exactly — the one behavior that cannot be pinned
// deterministically from outside.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/audio/impulse_response.hpp>

#include <cmath>
#include <limits>
#include <vector>

using pulp::audio::ImpulseResponseLoadOptions;
using pulp::audio::kMaxIrSampleRate;
using pulp::audio::kMinIrSampleRate;
using pulp::audio::read_impulse_response_pcm;

namespace {

double energy_of(const std::vector<float>& v) {
    double e = 0.0;
    for (float s : v) e += static_cast<double>(s) * s;
    return e;
}

} // namespace

TEST_CASE("IR PCM loader rejects degenerate buffers", "[impulse-response]") {
    const std::vector<float> pcm(64, 0.25f);

    // No buffer / no frames / no channels: nothing to build an IR from.
    REQUIRE_FALSE(read_impulse_response_pcm(nullptr, 64, 1, 48000.0, 48000.0));
    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), 0, 1, 48000.0, 48000.0));
    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), 64, 0, 48000.0, 48000.0));
}

// A hostile (or merely broken) buffer claiming a 500 MHz rate makes the Kaiser
// resampler demand ~10^9 taps — a multi-GB allocation and minutes of design work.
// It must be refused before the resampler ever sees it. Same for an absurdly low
// rate, which is not real audio either.
TEST_CASE("IR PCM loader refuses an implausible source rate", "[impulse-response]") {
    const std::vector<float> pcm(1024, 0.1f);

    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), pcm.size(), 1,
                                            kMinIrSampleRate - 1.0, 48000.0));
    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), pcm.size(), 1,
                                            kMaxIrSampleRate + 1.0, 48000.0));
    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), pcm.size(), 1,
                                            500'000'000.0, 48000.0));

    // Exactly at the window's edges is accepted (the bounds are inclusive).
    REQUIRE(read_impulse_response_pcm(pcm.data(), pcm.size(), 1,
                                      kMinIrSampleRate, kMinIrSampleRate));
}

// A non-positive rate is a missing rate, not a hostile one: the target falls back
// to 48 kHz and the source falls back to the target, so a browser that hands over
// a buffer without metadata still gets a usable IR instead of a nullopt.
TEST_CASE("IR PCM loader falls back for unspecified rates", "[impulse-response]") {
    ImpulseResponseLoadOptions opts;
    opts.normalize_unit_energy = false;
    std::vector<float> pcm(256, 0.0f);
    pcm[0] = 1.0f;   // a delta: any resample would smear it, so this pins "no resample"

    // src_rate 0 -> treated as the session rate: no resample, length preserved.
    auto ir = read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 0.0, 48000.0, opts);
    REQUIRE(ir);
    REQUIRE(ir->size() == pcm.size());
    REQUIRE((*ir)[0] == Catch::Approx(1.0f).margin(1e-6f));

    // target_sample_rate 0 -> 48 kHz; source is also 48 kHz, so again no resample.
    auto ir2 = read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 48000.0, 0.0, opts);
    REQUIRE(ir2);
    REQUIRE(ir2->size() == pcm.size());
}

// A ten-minute "IR" is a fat-finger (someone dropped a whole song on the plugin),
// and decoding + FFT-partitioning it would stall the host. The preflight rejects
// it on the duration the buffer actually claims.
TEST_CASE("IR PCM loader rejects an absurdly long buffer", "[impulse-response]") {
    ImpulseResponseLoadOptions opts;
    opts.reject_longer_than_seconds = 2.0;
    opts.max_seconds = 0.0;

    const std::vector<float> long_pcm(48000 * 3, 0.1f);   // 3 s at 48 kHz
    REQUIRE_FALSE(read_impulse_response_pcm(long_pcm.data(), long_pcm.size(), 1,
                                            48000.0, 48000.0, opts));

    // The same buffer at a higher source rate is only 1.5 s -> accepted. The check
    // is on duration, not sample count.
    REQUIRE(read_impulse_response_pcm(long_pcm.data(), long_pcm.size(), 1,
                                      96000.0, 96000.0, opts));

    // 0 disables the preflight entirely.
    opts.reject_longer_than_seconds = 0.0;
    REQUIRE(read_impulse_response_pcm(long_pcm.data(), long_pcm.size(), 1,
                                      48000.0, 48000.0, opts));
}

// A NaN/Inf sample would spread through every partition of the convolver's FFT and
// silence the plugin for the rest of the session. It is rejected whether or not
// normalization is on (normalization is not what catches it).
TEST_CASE("IR PCM loader rejects non-finite content", "[impulse-response]") {
    std::vector<float> pcm(128, 0.5f);
    pcm[64] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 48000.0, 48000.0));

    ImpulseResponseLoadOptions no_norm;
    no_norm.normalize_unit_energy = false;
    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 48000.0,
                                            48000.0, no_norm));

    pcm[64] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 48000.0, 48000.0));
}

// Zero energy is fatal ONLY when normalizing (it divides by the norm). A silent IR
// with normalization off is a valid, if pointless, silence.
TEST_CASE("IR PCM loader rejects a silent buffer only when normalizing",
          "[impulse-response]") {
    const std::vector<float> silence(256, 0.0f);

    REQUIRE_FALSE(read_impulse_response_pcm(silence.data(), silence.size(), 1,
                                            48000.0, 48000.0));

    ImpulseResponseLoadOptions no_norm;
    no_norm.normalize_unit_energy = false;
    auto ir = read_impulse_response_pcm(silence.data(), silence.size(), 1, 48000.0,
                                        48000.0, no_norm);
    REQUIRE(ir);
    REQUIRE(ir->size() == silence.size());
    REQUIRE(energy_of(*ir) == 0.0);
}

// The browser hands the planes of an AudioBuffer over concatenated: channels
// consecutive planes of `frames` samples each. A stereo IR is summed and averaged
// to mono (a true-stereo IR is a future extension).
TEST_CASE("IR PCM loader averages planar channels to mono", "[impulse-response]") {
    ImpulseResponseLoadOptions opts;
    opts.normalize_unit_energy = false;

    constexpr std::size_t kFrames = 32;
    std::vector<float> planar(kFrames * 2, 0.0f);
    for (std::size_t i = 0; i < kFrames; ++i) {
        planar[i] = 1.0f;                // plane 0 (L)
        planar[kFrames + i] = 0.0f;      // plane 1 (R), silent
    }

    auto ir = read_impulse_response_pcm(planar.data(), kFrames, 2, 48000.0, 48000.0,
                                        opts);
    REQUIRE(ir);
    REQUIRE(ir->size() == kFrames);
    for (float v : *ir) REQUIRE(v == Catch::Approx(0.5f).margin(1e-6f));  // (1+0)/2

    // Reading the SECOND plane at all is the part a stride bug would break: swap
    // the content and the mono result must swap with it.
    for (std::size_t i = 0; i < kFrames; ++i) {
        planar[i] = 0.0f;
        planar[kFrames + i] = 2.0f;
    }
    auto ir2 = read_impulse_response_pcm(planar.data(), kFrames, 2, 48000.0, 48000.0,
                                         opts);
    REQUIRE(ir2);
    for (float v : *ir2) REQUIRE(v == Catch::Approx(1.0f).margin(1e-6f));  // (0+2)/2

    // Mono is passed through un-scaled (no divide by 1).
    auto mono = read_impulse_response_pcm(planar.data() + kFrames, kFrames, 1,
                                          48000.0, 48000.0, opts);
    REQUIRE(mono);
    for (float v : *mono) REQUIRE(v == Catch::Approx(2.0f).margin(1e-6f));
}

TEST_CASE("IR PCM loader normalizes to unit energy", "[impulse-response]") {
    std::vector<float> pcm(64, 0.0f);
    pcm[0] = 4.0f;   // a loud delta: energy 16, so the norm must scale by 1/4
    pcm[1] = 3.0f;   // ... total energy 25 -> gain 1/5

    auto ir = read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 48000.0, 48000.0);
    REQUIRE(ir);
    REQUIRE(energy_of(*ir) == Catch::Approx(1.0).margin(1e-5));
    REQUIRE((*ir)[0] == Catch::Approx(0.8f).margin(1e-5f));   // 4/5
    REQUIRE((*ir)[1] == Catch::Approx(0.6f).margin(1e-5f));   // 3/5
}

// The cap is applied at the SOURCE rate, so the resampled result also fits it —
// a 2x upsample of an uncapped buffer would otherwise double the FFT partition
// count the convolver has to run every block.
TEST_CASE("IR PCM loader caps the length at max_seconds", "[impulse-response]") {
    ImpulseResponseLoadOptions opts;
    opts.normalize_unit_energy = false;
    opts.max_seconds = 0.25;

    const std::vector<float> pcm(48000, 0.5f);   // 1 s at 48 kHz
    auto ir = read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 48000.0, 48000.0,
                                        opts);
    REQUIRE(ir);
    REQUIRE(ir->size() == 12000);   // 0.25 s at the (unchanged) session rate

    // Shorter than the cap: untouched.
    const std::vector<float> shorter(4000, 0.5f);
    auto ir2 = read_impulse_response_pcm(shorter.data(), shorter.size(), 1, 48000.0,
                                         48000.0, opts);
    REQUIRE(ir2);
    REQUIRE(ir2->size() == shorter.size());

    // 0 disables the cap.
    opts.max_seconds = 0.0;
    auto ir3 = read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 48000.0, 48000.0,
                                         opts);
    REQUIRE(ir3);
    REQUIRE(ir3->size() == pcm.size());
}

// Resampling is what makes a 44.1 kHz cabinet IR the right LENGTH and TONE in a 48
// kHz session, and the group-delay trim is what keeps its onset where the file put
// it (an artificial leading silence would smear the dry/wet alignment).
TEST_CASE("IR PCM loader resamples to the session rate", "[impulse-response]") {
    ImpulseResponseLoadOptions opts;
    opts.normalize_unit_energy = false;
    opts.max_seconds = 0.0;

    // 0.1 s of DC at 24 kHz -> ~0.1 s at 48 kHz, i.e. about twice the samples.
    const std::vector<float> pcm(2400, 1.0f);
    auto ir = read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 24000.0, 48000.0,
                                        opts);
    REQUIRE(ir);
    // Exact length depends on the filter's tap count; the DURATION is the contract.
    const double seconds = static_cast<double>(ir->size()) / 48000.0;
    REQUIRE(seconds == Catch::Approx(0.1).margin(0.01));

    // The onset is not pushed back by the filter's group delay: an early sample is
    // already near the DC level, not sitting in a lead-in of silence.
    REQUIRE((*ir)[8] == Catch::Approx(1.0f).margin(0.05f));
    for (float v : *ir) REQUIRE(std::isfinite(v));

    // The cap still holds ACROSS a resample: truncation happens at the source rate
    // so the (longer) resampled result cannot exceed it at the target rate.
    opts.max_seconds = 0.05;
    auto capped = read_impulse_response_pcm(pcm.data(), pcm.size(), 1, 24000.0,
                                            48000.0, opts);
    REQUIRE(capped);
    REQUIRE(capped->size() <= static_cast<std::size_t>(0.05 * 48000.0));

    // Downsampling (96 kHz source into a 48 kHz session) halves it symmetrically.
    opts.max_seconds = 0.0;
    const std::vector<float> hi(9600, 1.0f);   // 0.1 s at 96 kHz
    auto down = read_impulse_response_pcm(hi.data(), hi.size(), 1, 96000.0, 48000.0,
                                          opts);
    REQUIRE(down);
    REQUIRE(static_cast<double>(down->size()) / 48000.0
            == Catch::Approx(0.1).margin(0.01));
}

// The shared tail's clamp at the TARGET rate. The entry points truncate at the
// SOURCE rate, which lands the resampled result AT the cap — resampler margin and
// rounding can nudge it a few samples past, and this is the guard that trims it.
// Driving finish_impulse_response() directly is the only deterministic way to pin
// it (the margin is not something a caller can dial in).
TEST_CASE("IR pipeline clamps the resampled length at the target rate",
          "[impulse-response]") {
    ImpulseResponseLoadOptions opts;
    opts.normalize_unit_energy = false;
    opts.max_seconds = 0.01;   // 480 frames at 48 kHz

    std::vector<float> mono(1000, 0.5f);   // already past the cap, same rate in/out
    auto ir = pulp::audio::detail::finish_impulse_response(std::move(mono), 48000.0,
                                                           48000.0, opts);
    REQUIRE(ir);
    REQUIRE(ir->size() == 480);
    for (float v : *ir) REQUIRE(v == Catch::Approx(0.5f).margin(1e-6f));

    // An empty mono buffer produces no IR rather than an empty one.
    REQUIRE_FALSE(pulp::audio::detail::finish_impulse_response({}, 48000.0, 48000.0,
                                                               opts));
}
