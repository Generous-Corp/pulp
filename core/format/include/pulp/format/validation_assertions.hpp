#pragma once

/// @file validation_assertions.hpp
/// Framework-agnostic correctness checks for plugin/example validation tests.
///
/// These helpers cover the recurring "did the DSP/state/MIDI actually behave"
/// questions that every example test re-implements by hand: output finiteness,
/// silence/non-silence, peak bounds, parameter normalize↔denormalize stability,
/// state save/load determinism, and MIDI/SysEx equality.
///
/// Each helper returns a CheckResult (a bool plus a human-readable message) so
/// it composes with any test framework — Catch2, GoogleTest, or a bare
/// `if (!result) { ... }`. The header intentionally does NOT depend on a test
/// framework, so it ships with the SDK and is reusable by downstream example
/// projects that consume the installed Pulp headers:
///
/// @code
/// auto out = harness.process_buffer(input, 2, 512);
/// REQUIRE(pulp::format::validation::check_finite(out));
/// REQUIRE(pulp::format::validation::check_any_nonzero(out));
/// REQUIRE(pulp::format::validation::check_state_round_trip(harness.host()));
/// @endcode
///
/// Screenshot content-floor checks are intentionally NOT duplicated here — use
/// pulp::view::ScreenshotContentStats::passes_content_floor() (which already
/// exists in core/view) so the format layer keeps no dependency on the view
/// layer.

#include <pulp/audio/buffer.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pulp::format::validation {

/// Outcome of a single validation check.
///
/// Convertible to bool so it drops straight into `REQUIRE(...)` /
/// `if (!result)`; the `message` describes the first failure for diagnostics.
struct CheckResult {
    bool ok = true;
    std::string message;

    explicit operator bool() const { return ok; }

    static CheckResult pass() { return {true, {}}; }
    static CheckResult fail(std::string why) { return {false, std::move(why)}; }
};

// ── Audio ────────────────────────────────────────────────────────────────

/// True when no sample is NaN or infinite. The single most common DSP smoke
/// check — a filter that blows up or a denormal path that returns inf is the
/// classic "it compiled but the audio is broken" bug.
inline CheckResult check_finite(std::span<const float> samples) {
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (!std::isfinite(samples[i])) {
            return CheckResult::fail(
                "non-finite sample at index " + std::to_string(i) + " (" +
                (std::isnan(samples[i]) ? "NaN" : "Inf") + ")");
        }
    }
    return CheckResult::pass();
}

/// @copydoc check_finite(std::span<const float>)
/// Templated on sample constness so it accepts both a mutable process()
/// output view (BufferView<float>) and a const input view.
template <typename Sample>
inline CheckResult check_finite(const audio::BufferView<Sample>& buffer) {
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        const auto channel = buffer.channel(ch);
        for (std::size_t i = 0; i < channel.size(); ++i) {
            if (!std::isfinite(channel[i])) {
                return CheckResult::fail(
                    "non-finite sample at ch " + std::to_string(ch) +
                    " index " + std::to_string(i) + " (" +
                    (std::isnan(channel[i]) ? "NaN" : "Inf") + ")");
            }
        }
    }
    return CheckResult::pass();
}

/// True when at least one sample's magnitude exceeds @p eps — i.e. the
/// processor actually produced signal. Catches the silent-output regression
/// where a gain stage or oscillator is wired but never writes.
inline CheckResult check_any_nonzero(std::span<const float> samples,
                                     float eps = 1.0e-7f) {
    for (float s : samples) {
        if (std::fabs(s) > eps) {
            return CheckResult::pass();
        }
    }
    return CheckResult::fail("all " + std::to_string(samples.size()) +
                            " samples are within +/-" + std::to_string(eps) +
                            " of zero (output is silent)");
}

/// True when every sample's magnitude is at or below @p eps — the inverse of
/// check_any_nonzero, for "bypass with zero input must be silent" style tests.
/// Fails closed on a non-finite sample (NaN/Inf is never "silent").
inline CheckResult check_silent(std::span<const float> samples,
                                float eps = 1.0e-7f) {
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (!std::isfinite(samples[i])) {
            return CheckResult::fail(
                "non-finite sample at index " + std::to_string(i));
        }
        if (std::fabs(samples[i]) > eps) {
            return CheckResult::fail(
                "expected silence but sample " + std::to_string(i) +
                " = " + std::to_string(samples[i]) + " exceeds +/-" +
                std::to_string(eps));
        }
    }
    return CheckResult::pass();
}

/// True when every sample's magnitude is at or below @p peak_limit (linear,
/// not dB). Use for "this effect must not exceed unity / must not clip" bounds.
/// Fails closed on a non-finite sample (a blown-up filter must not pass a
/// peak-bound check).
inline CheckResult check_peak_below(std::span<const float> samples,
                                    float peak_limit) {
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (!std::isfinite(samples[i])) {
            return CheckResult::fail(
                "non-finite sample at index " + std::to_string(i));
        }
        if (std::fabs(samples[i]) > peak_limit) {
            return CheckResult::fail(
                "sample " + std::to_string(i) + " = " +
                std::to_string(samples[i]) + " exceeds peak limit " +
                std::to_string(peak_limit));
        }
    }
    return CheckResult::pass();
}

// ── Parameters ──────────────────────────────────────────────────────────

/// Verify a parameter's normalize↔denormalize map is correct for @p raw.
///
/// Host automation lives in the normalized [0,1] domain; the round trip
/// real → normalize → denormalize must project @p raw onto a representable
/// value (honoring step quantization and skew). A naive
/// "denormalize(normalize(raw)) == raw" check spuriously fails on any quantized
/// or non-linear parameter, but an *idempotency-only* check is too weak — a
/// broken map that always returns 0.5 / a constant is idempotent and would
/// pass. So we check three independent things, fail-closed on any non-finite
/// result:
///   1. Finiteness + domain: normalize(raw) is finite and in [0,1];
///      denormalize is finite and in range.
///   2. An independent expected value for the common linear/stepped case
///      (clamp, then snap to step) — this catches a constant/garbage map.
///   3. For skewed ranges, the endpoint anchors (min↔0, max↔1) hold — a
///      constant map cannot satisfy both anchors unless min == max.
/// @p tol is an absolute tolerance in the parameter's own units; it is widened
/// internally by a small fraction of the range span so legitimate wide/skewed
/// ranges (e.g. 20..20000 Hz) don't false-fail on float precision.
inline CheckResult check_param_round_trip(const state::ParamRange& range,
                                          float raw, float tol = 1.0e-4f) {
    if (!std::isfinite(raw)) {
        return CheckResult::fail("raw input is not finite");
    }
    const float span = range.max - range.min;
    const float scale = std::max(1.0f, std::max(std::fabs(range.min),
                                                std::fabs(range.max)));
    const float eff_tol = std::max(tol, 1.0e-4f * scale);

    const float normalized = range.normalize(raw);
    if (!std::isfinite(normalized) || normalized < -eff_tol ||
        normalized > 1.0f + eff_tol) {
        return CheckResult::fail("normalize(" + std::to_string(raw) +
                                 ") = " + std::to_string(normalized) +
                                 " is not finite/in [0,1]");
    }
    const float projected = range.denormalize(normalized);
    if (!std::isfinite(projected) || projected < range.min - eff_tol ||
        projected > range.max + eff_tol) {
        return CheckResult::fail(
            "denormalized value " + std::to_string(projected) +
            " is not finite/in range [" + std::to_string(range.min) + ", " +
            std::to_string(range.max) + "]");
    }

    // Independent expectation for the linear/stepped case (no idempotency
    // self-reference): clamp raw into range, then snap to step. This is what a
    // correct linear denormalize must produce, so a constant/garbage map fails.
    if (range.is_linear()) {
        float expected = std::clamp(raw, range.min, range.max);
        if (range.step > 0.0f) {
            expected = range.min +
                       std::round((expected - range.min) / range.step) *
                           range.step;
            expected = std::clamp(expected, range.min, range.max);
        }
        if (std::fabs(projected - expected) > eff_tol) {
            return CheckResult::fail(
                "linear map wrong: expected " + std::to_string(expected) +
                ", got " + std::to_string(projected));
        }
        return CheckResult::pass();
    }

    // Skewed range: verify the endpoint anchors so a constant map can't pass.
    if (span > 0.0f) {
        const float at_zero = range.denormalize(0.0f);
        const float at_one = range.denormalize(1.0f);
        if (std::fabs(at_zero - range.min) > eff_tol ||
            std::fabs(at_one - range.max) > eff_tol) {
            return CheckResult::fail(
                "skewed map endpoints off: denorm(0)=" +
                std::to_string(at_zero) + " denorm(1)=" +
                std::to_string(at_one));
        }
    }
    // And that projecting the already-projected value is stable (rules out a
    // non-monotonic/garbage interior on top of correct anchors).
    const float reprojected = range.denormalize(range.normalize(projected));
    if (std::fabs(reprojected - projected) > eff_tol) {
        return CheckResult::fail(
            "skewed round trip not stable: " + std::to_string(projected) +
            " -> " + std::to_string(reprojected));
    }
    return CheckResult::pass();
}

// ── State ────────────────────────────────────────────────────────────────

/// Verify plugin state actually round-trips: save a snapshot, *perturb* every
/// parameter to a different value, restore the snapshot, and require the
/// re-saved bytes to match the original. The perturbation step is what makes
/// this catch the most common state bug — a load_state() that returns true but
/// silently ignores the blob. Without it, a no-op loader would "pass" because
/// the host's state never changed.
///
/// Covers the StateStore parameter portion generically. Custom (non-parameter)
/// plugin state from Processor::serialize_plugin_state() is exercised by the
/// save/load/save path but is not perturbed here, since its shape is unknown to
/// this helper; cover that with a processor-specific test if the plugin owns
/// custom state.
///
/// @note Mutates @p host. On success the host's parameter values are restored
/// to the original snapshot; on a detected bug they may be left perturbed.
inline CheckResult check_state_round_trip(HeadlessHost& host) {
    const std::vector<uint8_t> first = host.save_state();
    if (first.empty()) {
        return CheckResult::fail("save_state() produced an empty blob");
    }

    // Perturb every parameter to a value distinct from its current one so a
    // loader that ignores the blob is detectable. Pick the far end of the range
    // from the current value; skip degenerate (min == max) params.
    auto& store = host.state();
    bool perturbed_any = false;
    for (const auto& info : store.all_params()) {
        const auto& r = info.range;
        if (!(r.max > r.min)) {
            continue;
        }
        const float current = store.get_value(info.id);
        const float alt = (current - r.min) >= (r.max - current) ? r.min : r.max;
        if (alt != current) {
            store.set_value(info.id, alt);
            perturbed_any = true;
        }
    }

    if (!host.load_state(first)) {
        return CheckResult::fail("load_state() rejected a blob it just saved");
    }
    const std::vector<uint8_t> second = host.save_state();
    if (first != second) {
        return CheckResult::fail(
            "state not restored across save/perturb/load/save (" +
            std::to_string(first.size()) + " vs " +
            std::to_string(second.size()) +
            " bytes; load_state may be ignoring the blob)");
    }
    if (!perturbed_any) {
        // No automatable parameter could be perturbed (e.g. a pure custom-state
        // plugin). The check then only proves save determinism + that load
        // accepted its own blob — flag it so the caller knows coverage is thin.
        return CheckResult{true,
                           "save/load determinism only: no perturbable "
                           "parameters to prove load actually restores"};
    }
    return CheckResult::pass();
}

// ── MIDI / SysEx ──────────────────────────────────────────────────────────

/// True when two SysEx payloads carry identical bytes.
inline CheckResult check_sysex_payload_equal(std::span<const uint8_t> expected,
                                             std::span<const uint8_t> actual) {
    if (expected.size() != actual.size()) {
        return CheckResult::fail(
            "sysex length mismatch: expected " +
            std::to_string(expected.size()) + " bytes, got " +
            std::to_string(actual.size()));
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            return CheckResult::fail(
                "sysex byte " + std::to_string(i) + " mismatch: expected " +
                std::to_string(expected[i]) + ", got " +
                std::to_string(actual[i]));
        }
    }
    return CheckResult::pass();
}

/// True when two MIDI buffers carry the same short messages (bytes + sample
/// offset, in order), the same SysEx payloads (bytes + sample offset), and the
/// same UMP (MIDI 2.0) sidecar packets. Use to assert a MIDI-effect's output,
/// or a thru/echo path, matches expectation.
///
/// Compares the buffers as-laid-out; call sort() on both first if you only care
/// about sample-offset ordering rather than insertion order. This is
/// *block-event* equality: it compares each event's block-relative
/// `sample_offset` (the audio-block timing contract) and intentionally ignores
/// the absolute device-I/O `timestamp` field. For device/I/O paths where the
/// wall-clock timestamp matters, compare that separately.
inline CheckResult check_midi_events_equal(const midi::MidiBuffer& expected,
                                           const midi::MidiBuffer& actual) {
    if (expected.size() != actual.size()) {
        return CheckResult::fail(
            "short-message count mismatch: expected " +
            std::to_string(expected.size()) + ", got " +
            std::to_string(actual.size()));
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        const auto& a = actual[i];
        if (e.sample_offset != a.sample_offset) {
            return CheckResult::fail(
                "event " + std::to_string(i) + " sample_offset mismatch: " +
                std::to_string(e.sample_offset) + " vs " +
                std::to_string(a.sample_offset));
        }
        if (e.size() != a.size()) {
            return CheckResult::fail(
                "event " + std::to_string(i) + " byte-length mismatch: " +
                std::to_string(e.size()) + " vs " + std::to_string(a.size()));
        }
        for (uint32_t b = 0; b < e.size(); ++b) {
            if (e.data()[b] != a.data()[b]) {
                return CheckResult::fail(
                    "event " + std::to_string(i) + " byte " +
                    std::to_string(b) + " mismatch");
            }
        }
    }

    const auto& expected_sysex = expected.sysex();
    const auto& actual_sysex = actual.sysex();
    if (expected_sysex.size() != actual_sysex.size()) {
        return CheckResult::fail(
            "sysex count mismatch: expected " +
            std::to_string(expected_sysex.size()) + ", got " +
            std::to_string(actual_sysex.size()));
    }
    for (std::size_t i = 0; i < expected_sysex.size(); ++i) {
        if (expected_sysex[i].sample_offset != actual_sysex[i].sample_offset) {
            return CheckResult::fail(
                "sysex " + std::to_string(i) + " sample_offset mismatch");
        }
        const auto expected_bytes = expected_sysex[i].data.to_vector();
        const auto actual_bytes = actual_sysex[i].data.to_vector();
        auto byte_result = check_sysex_payload_equal(expected_bytes, actual_bytes);
        if (!byte_result) {
            return CheckResult::fail("sysex " + std::to_string(i) + ": " +
                                     byte_result.message);
        }
    }

    // UMP (MIDI 2.0) sidecar. A null buffer and an empty buffer are treated as
    // equivalent ("no UMP this block"); a non-empty mismatch must fail.
    const midi::UmpBuffer* expected_ump = expected.ump();
    const midi::UmpBuffer* actual_ump = actual.ump();
    const std::size_t expected_ump_count = expected_ump ? expected_ump->size() : 0;
    const std::size_t actual_ump_count = actual_ump ? actual_ump->size() : 0;
    if (expected_ump_count != actual_ump_count) {
        return CheckResult::fail(
            "UMP packet count mismatch: expected " +
            std::to_string(expected_ump_count) + ", got " +
            std::to_string(actual_ump_count));
    }
    for (std::size_t i = 0; i < expected_ump_count; ++i) {
        const auto& e = (*expected_ump)[i];
        const auto& a = (*actual_ump)[i];
        if (e.sample_offset != a.sample_offset) {
            return CheckResult::fail(
                "UMP " + std::to_string(i) + " sample_offset mismatch");
        }
        if (e.packet.word_count != a.packet.word_count) {
            return CheckResult::fail(
                "UMP " + std::to_string(i) + " word_count mismatch");
        }
        for (int w = 0; w < e.packet.word_count && w < 4; ++w) {
            if (e.packet.words[w] != a.packet.words[w]) {
                return CheckResult::fail("UMP " + std::to_string(i) +
                                         " word " + std::to_string(w) +
                                         " mismatch");
            }
        }
    }
    return CheckResult::pass();
}

} // namespace pulp::format::validation
