#pragma once

/// @file wavetable_authoring.hpp
/// Deterministic offline compilation of a mono recording into a band-limited
/// stack consumed by `pulp::signal::WavetableT`.
///
/// This API allocates and performs an exhaustive cycle/seam search. It is for
/// control or worker threads only and must not be called from an audio callback.

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_record_commit.hpp>
#include <pulp/signal/wavetable.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::audio {

inline constexpr std::uint32_t kWavetableAuthoringRecipeSchemaVersion = 1;
inline constexpr std::uint64_t kWavetableAuthoringMaximumSourceFrames = 1u << 20;
inline constexpr std::uint64_t kWavetableAuthoringMaximumAnalysisFrames = 1u << 16;
inline constexpr std::uint64_t kWavetableAuthoringMaximumCycleSearchWork = 1u << 25;

enum class WavetableCompileStatus : std::uint8_t {
    Ok,
    InvalidSource,
    InvalidRecipe,
    InvalidExplicitCycle,
    InsufficientSignal,
    NoReliableCycle,
    SizeOverflow,
    AllocationFailed,
    FftUnavailable,
    NonFiniteResult,
};

struct WavetableCycleSelection {
    std::uint64_t start_frame = 0;
    std::uint32_t length_frames = 0;
};

struct WavetableAuthoringRecipe {
    std::uint32_t schema_version = kWavetableAuthoringRecipeSchemaVersion;
    std::optional<WavetableCycleSelection> explicit_cycle;
    SampleHeritageAutoCycleOptions automatic_cycle;
    std::uint32_t table_length = 2048;
    std::uint32_t num_bands = 10;
    double reference_sample_rate = 48000.0;
    std::uint32_t guard_harmonics = 1;
    double normalize_target_dbfs = -1.0;
    /// Maximum accepted adjacent-period seam error after normalization by the
    /// selected cycle's RMS. The diagnostic combines level and slope error.
    double maximum_seam_error = 0.1;
};

struct WavetableAuthoringProvenance {
    SampleHeritageRecordProvenance source;
    std::string license_id;
    std::string rights_note;
};

struct WavetableCompileResult {
    WavetableCompileStatus status = WavetableCompileStatus::InvalidSource;
    std::vector<signal::WavetableEntry> bands;
    WavetableAuthoringRecipe recipe;
    WavetableAuthoringProvenance provenance;
    WavetableCycleSelection chosen_cycle;
    /// Estimator correlation for automatic selection; 0 for an explicit cycle,
    /// where no correlation estimate is performed.
    double cycle_correlation = 0.0;
    double source_seam_level_error = 0.0;
    double source_seam_slope_error = 0.0;
    /// Canonical source digest: domain tag, little-endian dimensions and sample
    /// rate, then channel-major IEEE-754 sample bits with negative zero folded.
    std::string source_audio_sha256;
    /// Canonical digest of the returned band snapshot: domain tag, ordered
    /// ceilings, dimensions, and IEEE-754 samples. Recipe and provenance are
    /// intentionally excluded. Callers that mutate `bands` must treat this
    /// digest as stale.
    std::string materialized_table_sha256;

    bool valid() const noexcept {
        return status == WavetableCompileStatus::Ok;
    }
};

/// Compile one mono source buffer into an owned band stack for WavetableT.
/// Computed hashes always replace, rather than trust, caller bookkeeping.
WavetableCompileResult compile_wavetable(BufferView<const float> source, double source_sample_rate,
                                         const WavetableAuthoringRecipe& recipe = {},
                                         const WavetableAuthoringProvenance& provenance = {});

} // namespace pulp::audio
