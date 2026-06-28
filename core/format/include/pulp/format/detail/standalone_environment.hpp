#pragma once

#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>

#include <charconv>
#include <string_view>
#include <system_error>

namespace pulp::format::detail {

inline bool standalone_env_truthy(std::string_view name) {
    return environment_flag_truthy(name);
}

inline bool parse_positive_frame_delay(std::string_view value, int& frames) {
    if (value.empty() || value.front() == '+') return false;

    int parsed = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed <= 0)
        return false;

    frames = parsed;
    return true;
}

inline bool parse_nonnegative_int(std::string_view value, int& out) {
    if (value.empty() || value.front() == '+') return false;

    int parsed = 0;
    const char* first = value.data();
    const char* last = first + value.size();
    auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed < 0)
        return false;

    out = parsed;
    return true;
}

inline StandaloneConfig standalone_config_from_environment(StandaloneConfig config) {
    if (standalone_env_truthy("PULP_HEADLESS")
        || standalone_env_truthy("PULP_TEST_MODE")
        || standalone_env_truthy("CI")) {
        config.headless = true;
    }

    if (auto screenshot = runtime::get_env("PULP_SCREENSHOT");
        screenshot && config.screenshot_path.empty()) {
        config.screenshot_path = *screenshot;
    }

    if (auto frames = runtime::get_env("PULP_FRAMES")) {
        int parsed = 0;
        if (parse_positive_frame_delay(*frames, parsed))
            config.screenshot_frame_delay = parsed;
    }

    // Programmatic live-probe readout for agents / CI. Parse the request even
    // when probes are compiled out so run_with_editor() can reject it with a
    // specific unsupported-build error instead of falling through to generic
    // headless/screenshot validation.
    if (auto probe_json = runtime::get_env("PULP_AUDIO_PROBE_JSON");
        probe_json && config.audio_probe_json_path.empty()) {
        config.audio_probe_json_path = *probe_json;
    }
    if (!config.audio_probe_json_path.empty())
        config.headless = true;

    if (auto scope_json = runtime::get_env("PULP_AUDIO_SCOPE_JSON");
        scope_json && config.audio_scope_json_path.empty()) {
        config.audio_scope_json_path = *scope_json;
    }
    if (auto window = runtime::get_env("PULP_AUDIO_SCOPE_WINDOW")) {
        int parsed = 0;
        if (parse_positive_frame_delay(*window, parsed))
            config.audio_scope_window_samples = parsed;
    }
    if (auto trigger = runtime::get_env("PULP_AUDIO_SCOPE_TRIGGER");
        trigger && !trigger->empty()) {
        config.audio_scope_trigger = *trigger;
    }
    if (auto channel = runtime::get_env("PULP_AUDIO_SCOPE_CHANNEL")) {
        int parsed = 0;
        if (parse_nonnegative_int(*channel, parsed))
            config.audio_scope_channel = parsed;
    }
    if (!config.audio_scope_json_path.empty())
        config.headless = true;

    if (auto capture_wav = runtime::get_env("PULP_AUDIO_CAPTURE_WAV");
        capture_wav && config.audio_capture_wav_path.empty()) {
        config.audio_capture_wav_path = *capture_wav;
    }
    if (auto frames = runtime::get_env("PULP_AUDIO_CAPTURE_WAV_FRAMES")) {
        int parsed = 0;
        if (parse_positive_frame_delay(*frames, parsed))
            config.audio_capture_wav_frames = parsed;
    }
    if (!config.audio_capture_wav_path.empty())
        config.headless = true;

    if (auto capture_rolling = runtime::get_env("PULP_AUDIO_CAPTURE_ROLLING");
        capture_rolling && config.audio_capture_rolling_path.empty()) {
        config.audio_capture_rolling_path = *capture_rolling;
    }
    if (auto frames = runtime::get_env("PULP_AUDIO_CAPTURE_ROLLING_FRAMES")) {
        int parsed = 0;
        if (parse_positive_frame_delay(*frames, parsed))
            config.audio_capture_rolling_frames = parsed;
    }
    if (auto fmt = runtime::get_env("PULP_AUDIO_CAPTURE_ROLLING_FORMAT")) {
        if (*fmt == "int24") {
            config.audio_capture_rolling_int24 = true;
        } else if (*fmt == "float") {
            config.audio_capture_rolling_int24 = false;
        } else {
            // Unlike the CLI flag (which hard-rejects), an env var can't fail the
            // launch — so warn loudly and default to float rather than silently
            // swallowing what is almost certainly a typo.
            runtime::log_warn(
                "Standalone: PULP_AUDIO_CAPTURE_ROLLING_FORMAT='{}' unrecognized "
                "(expected 'float' or 'int24'); using float",
                *fmt);
            config.audio_capture_rolling_int24 = false;
        }
    }
    if (!config.audio_capture_rolling_path.empty())
        config.headless = true;

    if (!config.screenshot_path.empty())
        config.headless = true;

    return config;
}

inline bool standalone_headless_requires_screenshot(const StandaloneConfig& config) {
    if (!config.headless) return false;
    if (!config.screenshot_path.empty()) return false;
#if PULP_ENABLE_AUDIO_PROBES
    if (!config.audio_probe_json_path.empty()) return false;
    if (!config.audio_scope_json_path.empty()) return false;
    if (!config.audio_capture_wav_path.empty()) return false;
    if (!config.audio_capture_rolling_path.empty()) return false;
#endif
    return true;
}

inline bool standalone_probe_json_requested_but_disabled(
    const StandaloneConfig& config) {
#if PULP_ENABLE_AUDIO_PROBES
    (void)config;
    return false;
#else
    return !config.audio_probe_json_path.empty()
        || !config.audio_scope_json_path.empty()
        || !config.audio_capture_wav_path.empty()
        || !config.audio_capture_rolling_path.empty();
#endif
}

}  // namespace pulp::format::detail
