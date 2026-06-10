#pragma once

/// @file audio_artifacts.hpp
/// JSON metrics artifacts for failing render scenarios (harness PR 1B).
///
/// When a scenario assertion fails, the test writes the analyzed
/// `BufferMetrics` as a JSON document so a failing local/CI run leaves a
/// machine-readable record of what the signal actually looked like
/// (peak/RMS/DC/NaN/clipping per channel) without re-running the render.
/// Serialized via choc::value + choc::json (CHOC-first policy). The schema
/// carries a `schema_version` field plus scenario provenance so downstream
/// tooling can evolve without guessing.

#include "audio_metrics.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace pulp::test::audio {

/// Current artifact schema version. Bump when fields change meaning or are
/// removed; purely additive fields do not require a bump.
inline constexpr std::int64_t kMetricsArtifactSchemaVersion = 1;

/// Serialize metrics to a JSON string. `scenario` is recorded verbatim as
/// provenance (which test scenario produced this buffer).
std::string metrics_to_json(const BufferMetrics& metrics,
                            std::string_view scenario);

/// Write metrics_to_json() to
/// `<temp>/pulp-audio-metrics/<sanitized-scenario>.json` (overwriting any
/// previous run's artifact for the same scenario) and return the path.
/// Scenario names are sanitized to [A-Za-z0-9._-] for the filename only.
/// Returns an empty path if the temp dir is unavailable or the open/write
/// fails — callers should treat an empty path as "no artifact written" rather
/// than reporting a phantom successful write.
std::filesystem::path write_metrics_artifact(const BufferMetrics& metrics,
                                             std::string_view scenario);

} // namespace pulp::test::audio
