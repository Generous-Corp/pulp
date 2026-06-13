#pragma once

#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/audio_scope.hpp>
#include <pulp/audio/audio_scope_json.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/standalone.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace pulp::format::detail {

inline bool write_audio_scope_json_snapshot(
    const std::string& path,
    const audio::AudioScopeResult& result) {
    if (path.empty()) return false;
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << audio::audio_scope_result_to_json(result, false);
    return out.good();
}

inline audio::AudioScopeResult build_audio_scope_result(
    audio::AudioProbe& probe,
    const StandaloneConfig& config) {
    audio::AudioScopeResult result;
    auto snapshot = probe.latest();
    result.stage = snapshot.stage_id;

    audio::AudioScopeTriggerMode trigger = audio::AudioScopeTriggerMode::kRisingZero;
    std::vector<std::string> pre_warnings;
    if (!audio::parse_audio_scope_trigger_mode(config.audio_scope_trigger, trigger)) {
        pre_warnings.push_back("invalid_trigger_mode");
    }

    constexpr int kMaxScopeWindowSamples = 16384;
    const int requested_window = std::clamp(config.audio_scope_window_samples,
                                           1, kMaxScopeWindowSamples);
    if (requested_window != config.audio_scope_window_samples)
        pre_warnings.push_back("window_clamped_to_max");

    const std::size_t source_channels =
        std::max<std::size_t>(1, snapshot.channel_count);
    audio::Buffer<float> samples(source_channels,
                                 static_cast<std::size_t>(requested_window));
    const int frames = probe.read_capture(samples.view(), requested_window);
    if (frames < requested_window)
        samples.resize(source_channels, static_cast<std::size_t>(std::max(frames, 0)));

    audio::AudioScopeAcquisitionConfig acq_config;
    acq_config.window_samples = static_cast<std::uint32_t>(requested_window);
    acq_config.trigger_mode = trigger;
    acq_config.selected_channel = static_cast<std::uint32_t>(
        std::max(config.audio_scope_channel, 0));
    const auto& const_samples = samples;
    result.acquisition = audio::acquire_audio_scope_window(const_samples.view(), acq_config,
                                                          &snapshot);
    result.acquisition.warnings.insert(result.acquisition.warnings.begin(),
                                       pre_warnings.begin(), pre_warnings.end());
    result.measurements = audio::measure_audio_scope_window(result.acquisition);
    return result;
}

inline bool write_audio_scope_json_file(const std::string& path,
                                        audio::AudioProbe& probe,
                                        const StandaloneConfig& config) {
    return write_audio_scope_json_snapshot(path,
                                           build_audio_scope_result(probe, config));
}

}  // namespace pulp::format::detail
