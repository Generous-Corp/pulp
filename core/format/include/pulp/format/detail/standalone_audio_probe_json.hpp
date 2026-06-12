#pragma once

#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/audio_probe_json.hpp>
#include <pulp/audio/audio_probe_snapshot.hpp>
#include <pulp/audio/audio_stats.hpp>
#include <pulp/runtime/log.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace pulp::format::detail {

#if PULP_ENABLE_AUDIO_PROBES

inline audio::AudioStats stats_for_probe_json_snapshot(
    const audio::AudioProbeSnapshot& snapshot) {
    audio::AudioStats stats;
    stats.callbacks = snapshot.callbacks;
    stats.clipped_blocks = snapshot.clipped_blocks;
    stats.nan_blocks = snapshot.nan_blocks;
    return stats;
}

inline bool write_audio_probe_json_snapshot(
    const std::string& path,
    const audio::AudioProbeSnapshot& snapshot) {
    if (path.empty()) return false;

    const auto json = audio::audio_probe_snapshot_to_json(
        snapshot, stats_for_probe_json_snapshot(snapshot));
    std::ofstream out(path, std::ios::binary);
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (out) {
        runtime::log_info("Standalone: audio probe JSON written to {}", path);
        return true;
    }

    runtime::log_error("Standalone: failed to write audio probe JSON to {}",
                       path);
    return false;
}

inline bool write_audio_probe_json_file(
    const std::string& path,
    audio::AudioProbe& probe) {
    return write_audio_probe_json_snapshot(path, probe.latest());
}

inline std::string audio_inspector_screenshot_path(
    const std::string& main_screenshot_path) {
    std::filesystem::path p(main_screenshot_path);
    return (p.parent_path() /
            (p.stem().string() + ".audio-inspector" + p.extension().string()))
        .string();
}

#endif

}  // namespace pulp::format::detail
