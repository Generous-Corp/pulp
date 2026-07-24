#pragma once

#include <pulp/format/detail/standalone_environment.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pulp::format::detail {

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
        StandaloneSettingsActions,
        std::vector<Processor::SettingsSection>,
        view::AudioBridge*);

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
    StandaloneSettingsActions actions,
    std::vector<Processor::SettingsSection> plugin_sections = {},
    view::AudioBridge* output_bridge = nullptr) {
    StandaloneEditorChrome chrome(std::move(editor_root));
    if (!config.show_settings_tab) {
        return chrome;
    }

    auto settings_panel = std::make_unique<SettingsPanel>();
    auto* settings_ptr = settings_panel.get();
    settings_panel->bind_systems(audio_system, midi_system);
    settings_panel->set_current_config(config);
    settings_panel->set_input_meter_bridge(input_bridge);
    settings_panel->set_output_meter_bridge(output_bridge);
    settings_panel->set_callbacks(
        make_standalone_settings_callbacks(*settings_panel, std::move(actions)));

    // Compose plugin-contributed settings tabs after the host-owned Audio/MIDI tabs.
    for (auto& section : plugin_sections)
        if (section.view) settings_panel->add_section(std::move(section.title), std::move(section.view));

    auto tab_panel = std::make_unique<view::TabPanel>();
    tab_panel->flex().flex_grow = 1.0f;
    // Make the editor tab FILL the (letterboxed) design area. TabPanel::add_tab
    // applies no flex to tab content, and FlexStyle defaults to flex_grow=0 in a
    // column, so a fill-based editor (one with no intrinsic height — e.g. a
    // design-viewport editor that lays out from local_bounds()) collapses to a
    // thin strip while the cross axis stretches. SettingsPanel avoids this by
    // setting flex_grow=1 on itself in its own ctor; do the same for the editor
    // root here. Harmless for editors that already size themselves (flex_grow
    // only claims remaining space). Fixes the Bendr-tracker #45 standalone squish.
    chrome.editor_root_->flex().flex_grow = 1.0f;
    // The window host fires `on_global_key` on the WINDOW ROOT only. Once the
    // editor is wrapped in this TabPanel the TabPanel becomes the window root,
    // so an editor that installed its own `on_global_key` (e.g. a sampler's ⌘K
    // keyboard toggle / musical typing, delivered host-wide via
    // performKeyEquivalent:) would never be reached. Forward the root-level
    // global keys down to the editor's handler so those shortcuts keep working
    // in the settings-chrome standalone exactly as they do when the editor is
    // hosted directly. `editor_raw` is owned by the TabPanel (as tab content)
    // for the lifetime of this callback, so the capture stays valid.
    view::View* editor_raw = chrome.editor_root_.get();
    tab_panel->add_tab("Editor", std::move(chrome.editor_root_));
    tab_panel->add_tab("Settings", std::move(settings_panel));
    if (editor_raw) {
        tab_panel->on_global_key = [editor_raw](const view::KeyEvent& event) -> bool {
            return editor_raw->on_global_key ? editor_raw->on_global_key(event) : false;
        };
    }
    // No outer [Editor][Settings] tab bar — the editor reaches Settings via its own
    // control (e.g. a gear button) and the Settings panel has a Done back to the editor.
    // This keeps one level of tabs (the panel's own Audio/MIDI/<plugin> tabs).
    tab_panel->set_show_tab_bar(false);

    chrome.window_root_ = tab_panel.get();
    chrome.settings_panel_ = settings_ptr;
    chrome.tab_panel_ = std::move(tab_panel);
    chrome.extra_window_height_ = 0.0f;  // no outer tab bar to reserve room for
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

inline void configure_standalone_tab_resizing(
    view::WindowHost& window,
    StandaloneEditorChrome& chrome,
    ViewBridge& bridge) {
    auto* tab_panel = chrome.tab_panel();
    if (!tab_panel) return;

    tab_panel->on_tab_change = [&window, tab_panel, &bridge](int index) {
        const auto& hints = bridge.size_hints();
        const float editor_w = static_cast<float>(hints.preferred_width);
        const float editor_h = static_cast<float>(hints.preferred_height);
        const bool settings = index == tab_panel->find_tab("Settings");
        const float height =
            settings
                ? std::max(
                      editor_h,
                      static_cast<float>(SettingsPanel::preferred_height()))
                : editor_h;
        window.set_fixed_aspect_ratio(editor_w / height);
        window.set_design_viewport(editor_w, height);
        window.request_content_size(editor_w, height);
    };
}

inline void install_standalone_editor_resize_handler(
    Processor& processor,
    const void* owner,
    view::WindowHost& window,
    ViewBridge& bridge) {
    processor.set_editor_resize_handler(
        owner,
        [&window, &bridge](uint32_t width, uint32_t height) -> bool {
            if (!bridge.set_preferred_size(width, height)) return false;

            const float w = static_cast<float>(width);
            const float h = static_cast<float>(height);
            // An owned native window has no host negotiation step:
            // request_content_size is synchronous on supported standalone
            // hosts, so publish it before committing the live viewport.
            window.request_content_size(w, h);
            window.set_fixed_aspect_ratio(w / h);
            window.set_design_viewport(w, h);
            return true;
        });
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

inline std::function<void()> make_standalone_idle_callback(
    std::function<void()> poll_scripted_ui,
    std::function<void()> poll_settings) {
    if (poll_scripted_ui) {
        return [poll_scripted_ui = std::move(poll_scripted_ui),
                poll_settings = std::move(poll_settings)] {
            poll_scripted_ui();
            if (poll_settings) {
                poll_settings();
            }
        };
    }

    return [poll_settings = std::move(poll_settings)] {
        if (poll_settings) {
            poll_settings();
        }
    };
}

inline void install_standalone_idle_callback(
    view::WindowHost& window,
    std::function<void()> poll_scripted_ui,
    std::function<void()> poll_settings) {
    window.set_idle_callback(make_standalone_idle_callback(
        std::move(poll_scripted_ui), std::move(poll_settings)));
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
