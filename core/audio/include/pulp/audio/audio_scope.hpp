#pragma once

/// @file audio_scope.hpp
/// UI-independent acquisition and measurement helpers for Audio Scope.
///
/// These helpers run on copied, non-realtime sample data. They never touch the
/// audio callback, never allocate on the realtime path, and do not own any UI
/// behavior. The live inspector, CLI, MCP, and future offline sources can all
/// share this one trigger/measurement implementation.

#include <pulp/audio/audio_probe_snapshot.hpp>
#include <pulp/audio/buffer.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::audio {

enum class AudioScopeTriggerMode : std::uint8_t {
    kNone = 0,
    kRisingZero = 1,
};

struct AudioScopeAcquisitionConfig {
    std::uint32_t window_samples = 2048;
    AudioScopeTriggerMode trigger_mode = AudioScopeTriggerMode::kRisingZero;
    std::uint32_t selected_channel = 0;
};

struct AudioScopeAcquisition {
    bool ok = false;
    double sample_rate = 0.0;
    std::uint32_t selected_channel = 0;
    std::uint32_t source_channel_count = 0;
    std::uint32_t source_frames = 0;
    std::uint32_t window_start = 0;
    std::uint32_t window_samples = 0;
    bool trigger_found = false;
    std::uint32_t trigger_sample = 0;
    std::uint64_t source_sequence_number = 0;
    std::vector<float> samples;
    std::vector<std::string> warnings;
};

struct AudioScopeMeasurements {
    bool peak_to_peak_available = false;
    double peak_to_peak = 0.0;
    bool rms_available = false;
    double rms = 0.0;
    bool dc_offset_available = false;
    double dc_offset = 0.0;
    bool crest_factor_available = false;
    double crest_factor = 0.0;
    bool frequency_available = false;
    double frequency_hz = 0.0;
    double period_samples = 0.0;
    std::vector<std::string> warnings;
};

struct AudioScopeResult {
    AudioProbeStage stage = AudioProbeStage::kUnknown;
    std::string source_kind = "live_probe";
    std::string source_path;
    AudioScopeTriggerMode trigger_mode = AudioScopeTriggerMode::kRisingZero;
    AudioScopeAcquisition acquisition;
    AudioScopeMeasurements measurements;
};

/// Zero-crossing frequency estimation is intentionally conservative. At least
/// two rising-zero intervals must agree within this fractional tolerance before
/// a frequency is reported.
inline constexpr double kAudioScopeFrequencyPeriodTolerance = 0.08;

AudioScopeAcquisition acquire_audio_scope_window(
    BufferView<const float> source,
    const AudioScopeAcquisitionConfig& config,
    const AudioProbeSnapshot* snapshot_metadata = nullptr);

AudioScopeMeasurements measure_audio_scope_window(
    const AudioScopeAcquisition& acquisition);

std::string_view audio_scope_trigger_mode_name(AudioScopeTriggerMode mode);

bool parse_audio_scope_trigger_mode(std::string_view text,
                                    AudioScopeTriggerMode& out);

}  // namespace pulp::audio
