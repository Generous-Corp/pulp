#pragma once

#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/view/window_host.hpp>

#include <charconv>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

    if (!config.screenshot_path.empty())
        config.headless = true;

    return config;
}

inline bool standalone_headless_requires_screenshot(const StandaloneConfig& config) {
    return config.headless && config.screenshot_path.empty();
}

struct StandaloneSettingsActions {
    std::function<bool(const StandaloneConfig&)> apply_config;
    std::function<void(SettingsPanel&)> rebind_after_apply;
    std::function<void(const TestSignalConfig&)> on_test_signal_changed;
    std::function<void(const std::string&)> on_file_load;
    std::function<void(bool play, bool loop)> on_file_transport;
};

inline SettingsPanelCallbacks make_standalone_settings_callbacks(
    SettingsPanel& settings_panel,
    StandaloneSettingsActions actions) {
    return SettingsPanelCallbacks{
        .on_config_apply =
            [apply_config = std::move(actions.apply_config),
             rebind_after_apply = std::move(actions.rebind_after_apply),
             settings_ptr = &settings_panel](const StandaloneConfig& cfg) {
                if (apply_config && apply_config(cfg) && rebind_after_apply) {
                    rebind_after_apply(*settings_ptr);
                }
            },
        .on_test_signal_changed =
            [on_test_signal_changed = std::move(actions.on_test_signal_changed)](
                const TestSignalConfig& cfg) {
                if (on_test_signal_changed) {
                    on_test_signal_changed(cfg);
                }
            },
        .on_file_load =
            [on_file_load = std::move(actions.on_file_load)](const std::string& path) {
                if (on_file_load) {
                    on_file_load(path);
                }
            },
        .on_file_transport =
            [on_file_transport = std::move(actions.on_file_transport)](bool play, bool loop) {
                if (on_file_transport) {
                    on_file_transport(play, loop);
                }
            },
    };
}

class StandaloneEditorChrome {
public:
    explicit StandaloneEditorChrome(std::unique_ptr<view::View> editor_root)
        : window_root_(editor_root.get()),
          editor_root_(std::move(editor_root)) {}

    StandaloneEditorChrome(const StandaloneEditorChrome&) = delete;
    StandaloneEditorChrome& operator=(const StandaloneEditorChrome&) = delete;
    StandaloneEditorChrome(StandaloneEditorChrome&&) noexcept = default;
    StandaloneEditorChrome& operator=(StandaloneEditorChrome&&) noexcept = default;

    view::View& window_root() const { return *window_root_; }
    SettingsPanel* settings_panel() const { return settings_panel_; }
    view::TabPanel* tab_panel() const { return tab_panel_.get(); }
    float extra_window_height() const { return extra_window_height_; }
    const char* chrome_label() const { return tab_panel_ ? "tabs" : "editor-only"; }

private:
    friend StandaloneEditorChrome make_standalone_editor_chrome(
        std::unique_ptr<view::View>,
        const StandaloneConfig&,
        audio::AudioSystem*,
        midi::MidiSystem*,
        view::AudioBridge*,
        StandaloneSettingsActions);

    view::View* window_root_ = nullptr;
    SettingsPanel* settings_panel_ = nullptr;
    std::unique_ptr<view::View> editor_root_;
    std::unique_ptr<view::TabPanel> tab_panel_;
    float extra_window_height_ = 0.0f;
};

inline StandaloneEditorChrome make_standalone_editor_chrome(
    std::unique_ptr<view::View> editor_root,
    const StandaloneConfig& config,
    audio::AudioSystem* audio_system,
    midi::MidiSystem* midi_system,
    view::AudioBridge* input_bridge,
    StandaloneSettingsActions actions) {
    StandaloneEditorChrome chrome(std::move(editor_root));
    if (!config.show_settings_tab) {
        return chrome;
    }

    auto settings_panel = std::make_unique<SettingsPanel>();
    auto* settings_ptr = settings_panel.get();
    settings_panel->bind_systems(audio_system, midi_system);
    settings_panel->set_current_config(config);
    settings_panel->set_input_meter_bridge(input_bridge);
    settings_panel->set_callbacks(
        make_standalone_settings_callbacks(*settings_panel, std::move(actions)));

    auto tab_panel = std::make_unique<view::TabPanel>();
    tab_panel->flex().flex_grow = 1.0f;
    tab_panel->add_tab("Editor", std::move(chrome.editor_root_));
    tab_panel->add_tab("Settings", std::move(settings_panel));

    chrome.window_root_ = tab_panel.get();
    chrome.settings_panel_ = settings_ptr;
    chrome.tab_panel_ = std::move(tab_panel);
    chrome.extra_window_height_ = 32.0f;
    return chrome;
}

/// Build a `WindowOptions` for the standalone editor host from the
/// processor's `ViewSize` hints (as cached by `ViewBridge`) plus the
/// chrome layout. The min_* fields propagate end-to-end so platform
/// hosts (e.g. macOS `setContentMinSize:`) can clamp interactive resize
/// to the processor's declared minimum. (closes #1362)
inline view::WindowOptions make_standalone_window_options(
    const ViewSize& size_hints,
    const StandaloneEditorChrome& chrome,
    std::string title,
    bool use_gpu) {
    const float extra_h = chrome.extra_window_height() > 0.0f
        ? chrome.extra_window_height() : 0.0f;
    view::WindowOptions opts;
    opts.title = std::move(title);
    opts.width = static_cast<float>(size_hints.preferred_width);
    opts.height = static_cast<float>(size_hints.preferred_height) + extra_h;
    // Forward min_* from the processor's ViewSize so the OS-level
    // window host can refuse interactive resize below the declared
    // minimum. Chrome rows (settings tab) extend the floor on the
    // height axis in lockstep with the preferred-height extension
    // above so the editor area can never shrink below its own min.
    if (size_hints.min_width > 0)
        opts.min_width = static_cast<float>(size_hints.min_width);
    if (size_hints.min_height > 0)
        opts.min_height = static_cast<float>(size_hints.min_height) + extra_h;
    opts.resizable = true;
    opts.use_gpu = use_gpu;
    return opts;
}

inline view::WindowHost::ContentSize standalone_editor_content_size(
    view::WindowHost::ContentSize host_content_size,
    const StandaloneEditorChrome& chrome) {
    const auto extra_height = static_cast<uint32_t>(
        chrome.extra_window_height() > 0.0f ? chrome.extra_window_height() : 0.0f);
    return {
        host_content_size.width,
        host_content_size.height > extra_height
            ? host_content_size.height - extra_height
            : 0u,
    };
}

template <typename ResizeFn>
inline view::WindowHost::ResizeCallback make_standalone_editor_resize_callback(
    const StandaloneEditorChrome& chrome,
    ResizeFn&& resize) {
    const auto extra_height = static_cast<uint32_t>(
        chrome.extra_window_height() > 0.0f ? chrome.extra_window_height() : 0.0f);
    return [extra_height, resize = std::forward<ResizeFn>(resize)](
               uint32_t width, uint32_t height) mutable {
        resize(width, height > extra_height ? height - extra_height : 0u);
    };
}

inline view::WindowHost::ContentSize standalone_design_viewport_size(
    const ViewSize& size_hints,
    const StandaloneEditorChrome& chrome) {
    const auto extra_height = static_cast<uint32_t>(
        chrome.extra_window_height() > 0.0f ? chrome.extra_window_height() : 0.0f);
    return {
        size_hints.preferred_width,
        size_hints.preferred_height + extra_height,
    };
}

inline void configure_standalone_design_viewport(
    view::WindowHost& window,
    const ViewSize& size_hints,
    const StandaloneEditorChrome& chrome) {
    const auto viewport = standalone_design_viewport_size(size_hints, chrome);
    if (viewport.width == 0 || viewport.height == 0)
        return;

    window.set_design_viewport(
        static_cast<float>(viewport.width),
        static_cast<float>(viewport.height));
    window.set_fixed_aspect_ratio(
        static_cast<float>(viewport.width) /
        static_cast<float>(viewport.height));
}

template <typename ResizeFn>
inline void sync_standalone_editor_host(
    view::WindowHost& window,
    const StandaloneEditorChrome& chrome,
    ResizeFn&& resize) {
    auto callback = make_standalone_editor_resize_callback(
        chrome, std::forward<ResizeFn>(resize));
    const auto host_content_size = window.get_content_size();
    window.set_resize_callback(callback);
    if (host_content_size.width > 0 && host_content_size.height > 0) {
        callback(host_content_size.width, host_content_size.height);
    }
}

template <typename Bridge>
inline void attach_standalone_editor_bridge(
    view::WindowHost& window,
    const StandaloneEditorChrome& chrome,
    Bridge& bridge) {
    sync_standalone_editor_host(
        window,
        chrome,
        [&bridge](uint32_t width, uint32_t height) {
            bridge.resize(width, height);
        });
}

inline void install_standalone_idle_callback(
    view::WindowHost& window,
    std::function<void()> poll_scripted_ui,
    std::function<void()> poll_settings) {
    if (poll_scripted_ui) {
        window.set_idle_callback(
            [poll_scripted_ui = std::move(poll_scripted_ui),
             poll_settings = std::move(poll_settings)] {
                poll_scripted_ui();
                if (poll_settings) {
                    poll_settings();
                }
            });
        return;
    }

    window.set_idle_callback([poll_settings = std::move(poll_settings)] {
        if (poll_settings) {
            poll_settings();
        }
    });
}

template <typename ScriptedUi>
inline void install_scripted_ui_repaint_callback(
    ScriptedUi* scripted_ui,
    view::WindowHost& window) {
    if (!scripted_ui) {
        return;
    }

    scripted_ui->set_repaint_callback([&window] {
        window.repaint();
    });
}

template <typename ScriptedUi, typename SettingsPoller>
inline void install_standalone_idle_callback(
    view::WindowHost& window,
    ScriptedUi* scripted_ui,
    SettingsPoller* settings_poller) {
    install_standalone_idle_callback(
        window,
        scripted_ui
            ? std::function<void()>{[scripted_ui] { scripted_ui->poll(); }}
            : std::function<void()>{},
        settings_poller
            ? std::function<void()>{[settings_poller] { settings_poller->poll(); }}
            : std::function<void()>{});
}

inline void log_standalone_window_open(
    uint32_t width,
    uint32_t height,
    bool use_gpu,
    bool uses_script_ui,
    const StandaloneEditorChrome& chrome) {
    runtime::log_info(
        "Standalone: editor window open ({}x{}, gpu={}, mode={}, chrome={}, inspector=ready)",
        width,
        height,
        use_gpu,
        uses_script_ui ? "scripted" : "autoui",
        chrome.chrome_label());
}

} // namespace pulp::format::detail
