#pragma once

#include <pulp/format/detail/delayed_action.hpp>
#include <pulp/format/standalone.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace pulp::format::detail {

enum class StandaloneOneShotActionKind {
    none,
    audio_probe_json,
    audio_scope_json,
    audio_capture_wav,
    audio_capture_rolling,
};

struct StandaloneOneShotActionRequest {
    StandaloneOneShotActionKind kind = StandaloneOneShotActionKind::none;
    std::string path;
    int delay = 30;
};

inline int standalone_one_shot_delay(const StandaloneConfig& config) {
    return config.screenshot_frame_delay > 0
        ? config.screenshot_frame_delay
        : 30;
}

inline StandaloneOneShotActionRequest
standalone_audio_one_shot_action_request(const StandaloneConfig& config) {
    StandaloneOneShotActionRequest request;
    request.delay = standalone_one_shot_delay(config);

    if (!config.screenshot_path.empty()) {
        return request;
    }

    if (!config.audio_probe_json_path.empty()) {
        request.kind = StandaloneOneShotActionKind::audio_probe_json;
        request.path = config.audio_probe_json_path;
    } else if (!config.audio_scope_json_path.empty()) {
        request.kind = StandaloneOneShotActionKind::audio_scope_json;
        request.path = config.audio_scope_json_path;
    } else if (!config.audio_capture_wav_path.empty()) {
        request.kind = StandaloneOneShotActionKind::audio_capture_wav;
        request.path = config.audio_capture_wav_path;
    } else if (!config.audio_capture_rolling_path.empty()) {
        request.kind = StandaloneOneShotActionKind::audio_capture_rolling;
        request.path = config.audio_capture_rolling_path;
    }

    return request;
}

inline std::function<void()> make_standalone_one_shot_idle_callback(
    std::function<void()> prior,
    DelayedAction action) {
    return [prior = std::move(prior), action = std::move(action)]() mutable {
        if (prior) {
            prior();
        }
        action();
    };
}

inline std::function<void()> make_standalone_one_shot_action(
    StandaloneOneShotActionRequest request,
    std::function<void(const std::string&)> write_probe_json,
    std::function<void(const std::string&)> write_scope_json,
    std::function<void(const std::string&)> write_capture_wav,
    std::function<void(const std::string&)> write_capture_rolling) {
    return [request = std::move(request),
            write_probe_json = std::move(write_probe_json),
            write_scope_json = std::move(write_scope_json),
            write_capture_wav = std::move(write_capture_wav),
            write_capture_rolling = std::move(write_capture_rolling)] {
        switch (request.kind) {
        case StandaloneOneShotActionKind::audio_probe_json:
            if (write_probe_json) write_probe_json(request.path);
            break;
        case StandaloneOneShotActionKind::audio_scope_json:
            if (write_scope_json) write_scope_json(request.path);
            break;
        case StandaloneOneShotActionKind::audio_capture_wav:
            if (write_capture_wav) write_capture_wav(request.path);
            break;
        case StandaloneOneShotActionKind::audio_capture_rolling:
            if (write_capture_rolling) write_capture_rolling(request.path);
            break;
        case StandaloneOneShotActionKind::none:
            break;
        }
    };
}

inline std::string_view standalone_one_shot_action_log_name(
    StandaloneOneShotActionKind kind) {
    switch (kind) {
    case StandaloneOneShotActionKind::audio_probe_json:
        return "audio-probe-json";
    case StandaloneOneShotActionKind::audio_scope_json:
        return "audio-scope-json";
    case StandaloneOneShotActionKind::audio_capture_wav:
        return "audio-capture-wav";
    case StandaloneOneShotActionKind::audio_capture_rolling:
        return "audio-capture-rolling";
    case StandaloneOneShotActionKind::none:
        return "none";
    }
}

}  // namespace pulp::format::detail
