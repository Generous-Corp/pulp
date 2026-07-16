#pragma once

/// @file audio_doctor_artifacts.hpp
/// JSON curve artifacts for the offline Audio Doctor.
///
/// The Doctor's response/THD curves are its reviewable outputs: a human, CI,
/// or future AI tool can read a stable JSON document instead of re-running the
/// analysis. Each artifact carries a `schema_version`, the analyzer name, the
/// Analyzer Determinism Contract fields (window, length, stimulus, sample
/// rate — so a reader can reproduce or judge the measurement), and the curve
/// points. Serialized via choc::value + choc::json (CHOC-first policy),
/// matching the audio_artifacts.cpp style.
///
/// Layering: this serializes the buffer-level analyzers' result structs, so it
/// includes audio_spectrum.hpp (the FFT-bearing analysis core). It lives in the
/// tool-only `pulp-audio-analysis` lib and must never be linked into a
/// runtime/plugin build.

#include "audio_spectrum.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace pulp::test::audio {

/// Current Doctor-curve schema version. Bump when a field changes meaning or
/// is removed; purely additive fields do not require a bump.
inline constexpr std::int64_t kDoctorCurveSchemaVersion = 1;

/// Serialize a magnitude/response curve to a JSON string. `scenario` is
/// recorded verbatim as provenance.
std::string response_curve_to_json(const ResponseCurve& curve,
                                   std::string_view scenario);

/// Serialize a THD result to a JSON string (including the harmonic breakdown).
std::string thd_to_json(const ThdResult& thd, std::string_view scenario);

/// Serialize a phase/group-delay curve to a JSON string.
///
/// Undefined points (stopband — see `PhaseCurve`) carry `"defined": false` and
/// **omit** `phase_rad` / `group_delay_samples` / `group_delay_seconds`
/// entirely: their in-memory value is NaN, which JSON cannot represent, and
/// writing a placeholder would hand a reader a number the analyzer never
/// measured. Read `defined` before reaching for the delay fields.
///
/// The magnitude key is `magnitude_db_rel_peak`, not the `.response.json`
/// artifact's `magnitude_db`, because the two are different quantities: this
/// one is normalized to the phase curve's own peak transfer magnitude, while
/// `.response.json`'s is the absolute out/in transfer ratio. A processor with
/// 12 dB of passband gain reports +12 in the sibling artifact and 0 here.
std::string phase_curve_to_json(const PhaseCurve& curve,
                                std::string_view scenario);

/// Write `response_curve_to_json` to
/// `<temp>/pulp-audio-doctor/<sanitized-scenario>.response.json` and return
/// the path (overwriting any previous run's artifact for the same scenario).
std::filesystem::path write_response_artifact(const ResponseCurve& curve,
                                              std::string_view scenario);

/// Write `thd_to_json` to
/// `<temp>/pulp-audio-doctor/<sanitized-scenario>.thd.json` and return path.
std::filesystem::path write_thd_artifact(const ThdResult& thd,
                                         std::string_view scenario);

/// Write `phase_curve_to_json` to
/// `<temp>/pulp-audio-doctor/<sanitized-scenario>.groupdelay.json` and return
/// the path.
std::filesystem::path write_phase_artifact(const PhaseCurve& curve,
                                           std::string_view scenario);

} // namespace pulp::test::audio
