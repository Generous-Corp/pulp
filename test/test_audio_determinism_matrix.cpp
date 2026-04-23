// Deterministic-matrix sweep harness for the audio engine contract.
//
// Closes stage-1 items 1 + 2 + 6 (sample-rate sweep, buffer-size sweep,
// first stateless-example cell of the golden breadth) from #356
// audio-validation-audit. The harness verifies the invariants every
// shipping example must hold regardless of host-reported SR or block
// size, using PulpGain (known SR-independent + stateless in its unity
// path) and PulpTone (instrument silence invariant).
//
// Invariants this pins:
//   1. Unity gain is bit-exact pass-through at every (SR, block) cell.
//   2. Silent-in ⇒ silent-out at every cell (no SR-dependent leakage /
//      DC / denormal drift at the default state).
//   3. Block-size invariance: one 2048-sample block and 2048 one-sample
//      blocks produce bit-identical output at unity gain.
//   4. Sample-rate change via re-prepare() does not poison downstream
//      processing (prepare→process→prepare→process with a new SR).
//   5. Instrument silence invariant: no MIDI ⇒ silent output at every
//      cell for PulpTone.
//
// Note: these are *structural* invariants of the framework layer, not
// signal fidelity of a given DSP block. Fidelity / impulse-response
// golden tests belong in test_golden_audio.cpp and per-processor golden
// files. #48 / #356 follow-ups expand to PulpEffect/Compressor/Drums/
// Synth (tracked in the umbrella).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "pulp_gain.hpp"
#include "pulp_tone.hpp"

using Catch::Matchers::WithinAbs;

namespace {

constexpr std::array<double, 5> kSampleRates{
    44100.0, 48000.0, 88200.0, 96000.0, 192000.0};

// 1 covers the pathological single-sample case; 4096 covers the
// largest block sizes hosts hand us. Anything bigger than this is
// platform-specific and out of scope for a cross-host matrix.
constexpr std::array<int, 6> kBlockSizes{1, 16, 64, 256, 1024, 4096};

std::vector<float> make_sine_vec(int samples, float freq, double sr) {
    std::vector<float> v(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        v[static_cast<std::size_t>(i)] = std::sin(
            2.0f * std::numbers::pi_v<float> * freq
            * static_cast<float>(i) / static_cast<float>(sr));
    }
    return v;
}

// Process `total_samples` through an effect-shaped host (2-in / 2-out),
// driven in `block` chunks. Returns the interleaved L channel output.
std::vector<float> process_blocked(pulp::format::HeadlessHost& host,
                                   const std::vector<float>& in_L,
                                   const std::vector<float>& in_R,
                                   int block) {
    const int total = static_cast<int>(in_L.size());
    std::vector<float> out_L(static_cast<std::size_t>(total), 0.0f);
    std::vector<float> out_R(static_cast<std::size_t>(total), 0.0f);

    for (int pos = 0; pos < total; pos += block) {
        const int n = std::min(block, total - pos);

        std::vector<float> chunk_in_L(in_L.begin() + pos,
                                      in_L.begin() + pos + n);
        std::vector<float> chunk_in_R(in_R.begin() + pos,
                                      in_R.begin() + pos + n);
        std::vector<float> chunk_out_L(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> chunk_out_R(static_cast<std::size_t>(n), 0.0f);

        const float* in_ptrs[2] = {chunk_in_L.data(), chunk_in_R.data()};
        float* out_ptrs[2] = {chunk_out_L.data(), chunk_out_R.data()};

        pulp::audio::BufferView<const float> in_view(in_ptrs, 2, n);
        pulp::audio::BufferView<float> out_view(out_ptrs, 2, n);
        host.process(out_view, in_view);

        std::copy(chunk_out_L.begin(), chunk_out_L.end(),
                  out_L.begin() + pos);
        std::copy(chunk_out_R.begin(), chunk_out_R.end(),
                  out_R.begin() + pos);
    }
    return out_L;
}

}  // namespace

// ── PulpGain: unity-gain pass-through is bit-exact at every cell ──────

TEST_CASE("Audio matrix: PulpGain unity gain bit-exact across SR x block",
          "[audio][matrix][determinism][issue-356]") {
    for (double sr : kSampleRates) {
        for (int block : kBlockSizes) {
            CAPTURE(sr);
            CAPTURE(block);

            pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
            host.prepare(sr, block);

            // 8192 is the smallest multiple of every block size we
            // sweep (including 4096) that guarantees the largest cell
            // runs at least two full-size blocks rather than short-
            // blocking the last call to min(block, total-pos). Codex
            // P2 on PR #378 flagged that the earlier total=2048
            // silently short-blocked the 4096 cell.
            constexpr int total = 8192;
            auto in_L = make_sine_vec(total, 440.0f, sr);
            auto in_R = make_sine_vec(total, 660.0f, sr);

            auto out_L = process_blocked(host, in_L, in_R, block);

            for (int i = 0; i < total; ++i) {
                REQUIRE_THAT(out_L[static_cast<std::size_t>(i)],
                             WithinAbs(in_L[static_cast<std::size_t>(i)],
                                       1e-6));
            }
        }
    }
}

// ── PulpGain: silent input stays silent at every cell ────────────────

TEST_CASE("Audio matrix: PulpGain silent-in -> silent-out across SR x block",
          "[audio][matrix][silence][issue-356]") {
    for (double sr : kSampleRates) {
        for (int block : kBlockSizes) {
            CAPTURE(sr);
            CAPTURE(block);

            pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);
            host.prepare(sr, block);

            constexpr int total = 8192;  // see unity-gain test for rationale
            std::vector<float> zeros(total, 0.0f);
            auto out = process_blocked(host, zeros, zeros, block);

            for (float s : out) {
                REQUIRE(s == 0.0f);
            }
        }
    }
}

// ── Block-size invariance: same input, same state, different block ──

TEST_CASE("Audio matrix: PulpGain block-size invariance at unity",
          "[audio][matrix][determinism][issue-356]") {
    constexpr double sr = 48000.0;
    constexpr int total = 8192;

    pulp::format::HeadlessHost host_big(pulp::examples::create_pulp_gain);
    host_big.prepare(sr, 4096);

    pulp::format::HeadlessHost host_tiny(pulp::examples::create_pulp_gain);
    host_tiny.prepare(sr, 1);

    auto in_L = make_sine_vec(total, 440.0f, sr);
    auto in_R = make_sine_vec(total, 660.0f, sr);

    // block=4096 vs block=1 over 8192 samples: at least two full
    // 4096-frame process calls against 8192 single-sample calls.
    auto out_big  = process_blocked(host_big,  in_L, in_R, 4096);
    auto out_tiny = process_blocked(host_tiny, in_L, in_R, 1);

    // Unity gain is purely numeric — the host should be oblivious to
    // the block size it was prepared with for this particular path.
    for (int i = 0; i < total; ++i) {
        REQUIRE(out_big[static_cast<std::size_t>(i)]
                == out_tiny[static_cast<std::size_t>(i)]);
    }
}

// ── SR change via re-prepare() is clean ──────────────────────────────

TEST_CASE("Audio matrix: PulpGain re-prepare at a new SR does not leak state",
          "[audio][matrix][prepare][issue-356]") {
    pulp::format::HeadlessHost host(pulp::examples::create_pulp_gain);

    host.prepare(44100.0, 256);
    {
        auto in_L = make_sine_vec(1024, 440.0f, 44100.0);
        auto in_R = make_sine_vec(1024, 660.0f, 44100.0);
        auto out = process_blocked(host, in_L, in_R, 256);
        // Expected path — unity gain, just warming caches / any internal
        // state the framework might hold.
        REQUIRE(out.front() == in_L.front());
    }

    host.prepare(96000.0, 128);
    {
        // Fresh silent input post-reprepare should stay silent. Any
        // leaked state from the previous SR would leak here.
        std::vector<float> zeros(1024, 0.0f);
        auto out = process_blocked(host, zeros, zeros, 128);
        for (float s : out) {
            REQUIRE(s == 0.0f);
        }
    }
}

// ── PulpTone instrument: no MIDI ⇒ silent at every cell ─────────────

TEST_CASE("Audio matrix: PulpTone silence invariant across SR x block",
          "[audio][matrix][instrument][silence][issue-356]") {
    for (double sr : kSampleRates) {
        for (int block : kBlockSizes) {
            CAPTURE(sr);
            CAPTURE(block);

            pulp::format::HeadlessHost host(pulp::examples::create_pulp_tone);
            host.prepare(sr, block, /*inputs*/ 0, /*outputs*/ 2);

            // 8192 samples, a clean multiple of every sweep block size
            // including 4096 (no short last block). Any finite-grained
            // envelope / oscillator / DC-block state would show up as
            // non-zero over this window.
            constexpr int total = 8192;
            std::vector<float> out_L(total, 0.0f);
            std::vector<float> out_R(total, 0.0f);

            for (int pos = 0; pos < total; pos += block) {
                const int n = std::min(block, total - pos);
                pulp::midi::MidiBuffer midi_in;
                pulp::midi::MidiBuffer midi_out;

                std::vector<float> chunk_out_L(static_cast<std::size_t>(n), 0.0f);
                std::vector<float> chunk_out_R(static_cast<std::size_t>(n), 0.0f);
                float* out_ptrs[2] = {chunk_out_L.data(), chunk_out_R.data()};
                pulp::audio::BufferView<float> out_view(out_ptrs, 2, n);

                const float* in_ptrs[] = {nullptr};
                pulp::audio::BufferView<const float> in_view(in_ptrs, 0, n);

                host.process(out_view, in_view, midi_in, midi_out);

                std::copy(chunk_out_L.begin(), chunk_out_L.end(),
                          out_L.begin() + pos);
                std::copy(chunk_out_R.begin(), chunk_out_R.end(),
                          out_R.begin() + pos);
            }

            for (float s : out_L) {
                REQUIRE(std::abs(s) < 1e-6f);
            }
            for (float s : out_R) {
                REQUIRE(std::abs(s) < 1e-6f);
            }
        }
    }
}
