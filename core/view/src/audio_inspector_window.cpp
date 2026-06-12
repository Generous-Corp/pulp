#include <pulp/view/audio_inspector_window.hpp>

#include <pulp/view/window_manager.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace pulp::view {

namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool env_false(std::string value) {
    value = lower_ascii(std::move(value));
    return value == "0" || value == "false" || value == "off" || value == "no";
}

constexpr const char* kAudioInspectorModePreference = "audio_inspector.mode";

std::string mode_pref_value(AudioInspectorMode mode) {
    return mode == AudioInspectorMode::kScope ? "scope" : "signal";
}

AudioInspectorMode parse_mode_pref(std::string value) {
    value = lower_ascii(std::move(value));
    if (value == "scope" || value == "oscilloscope")
        return AudioInspectorMode::kScope;
    return AudioInspectorMode::kSignal;
}

}  // namespace

AudioInspectorWindow::AudioInspectorWindow(WindowManager* mgr,
                                           WindowHost* parent,
                                           HostFactory host_factory,
                                           std::string app_name)
    : manager_(mgr)
    , parent_host_(parent)
    , host_factory_(std::move(host_factory))
    , app_name_(std::move(app_name)) {
    waveform_scratch_.resize(AudioWaveformView::kCapacity);
    app_properties_ = std::make_unique<state::ApplicationProperties>(
        app_name_.empty() ? "Pulp" : app_name_);
    app_properties_->load();
    preferences_ = &app_properties_->user_settings();
    build_ui();
    load_mode_preference();
}

AudioInspectorWindow::~AudioInspectorWindow() {
    // RAII handler removal — the registry must never be left holding a pointer
    // to a destroyed tool (dangling-handler dispatch is a UAF). Mirrors
    // InspectorWindow's destructor.
    if (registry_) {
        registry_->remove_handler(this);
        registry_ = nullptr;
    }
    if (manager_ && window_id_ != 0) {
        manager_->unregister_window(window_id_);
    }
}

void AudioInspectorWindow::build_ui() {
    root_ = std::make_unique<View>();
    root_->set_id("audio-inspector-window-root");
    root_->flex().direction = FlexDirection::column;
    root_->set_background_color(canvas::Color::rgba8(26, 26, 32, 255));

    auto panel = std::make_unique<AudioInspectorPanel>();
    panel->flex().flex_grow = 1;
    panel->on_mode_changed = [this](AudioInspectorMode mode) {
        if (applying_mode_) return;
        apply_mode(mode, true);
    };
    panel_ = panel.get();
    root_->add_child(std::move(panel));

    // Start in the honest "no probe" state.
    panel_->update(AudioInspectorPanel::Status::kNoProbe, {}, {}, nullptr, 0);
    panel_->set_mode(mode_);

    if (const char* trigger = std::getenv("PULP_AUDIO_INSPECTOR_TRIGGER")) {
        const auto value = lower_ascii(trigger);
        if (value == "rising-zero" || value == "rising_zero" ||
            value == "zero" || value == "1") {
            panel_->set_waveform_trigger_mode(
                AudioWaveformView::TriggerMode::kRisingZero);
        }
    }
    if (const char* grid = std::getenv("PULP_AUDIO_INSPECTOR_GRID")) {
        panel_->set_waveform_grid_visible(!env_false(grid));
    }
    if (const char* scale = std::getenv("PULP_AUDIO_INSPECTOR_SCALE")) {
        char* end = nullptr;
        const float parsed = std::strtof(scale, &end);
        if (end != scale)
            panel_->set_waveform_horizontal_scale(parsed);
    }
}

void AudioInspectorWindow::set_preferences(state::PropertiesFile* preferences) {
    preferences_ = preferences ? preferences : (app_properties_
        ? &app_properties_->user_settings() : nullptr);
    load_mode_preference();
}

void AudioInspectorWindow::set_probe(audio::AudioProbe* probe) {
    probe_ = probe;
    last_sequence_ = 0;
    ever_observed_ = false;
    if (!probe_ && panel_) {
        panel_->update(AudioInspectorPanel::Status::kNoProbe, {}, {}, nullptr, 0);
        panel_->clear_scope_result();
    }
}

void AudioInspectorWindow::set_device_stats(const audio::AudioStats& device_stats) {
    device_stats_ = device_stats;
}

void AudioInspectorWindow::poll() {
    if (!panel_) return;

    if (!probe_) {
        panel_->update(AudioInspectorPanel::Status::kNoProbe, {}, {}, nullptr, 0);
        return;
    }

    // Single-consumer TripleBuffer: read exactly once per tick.
    const audio::AudioProbeSnapshot snap = probe_->latest();

    // Stale detection. The producer publishes monotonic sequence numbers
    // starting at 1; a never-published probe reads back 0. "Live" means the
    // sequence advanced since our last poll.
    bool live = false;
    if (snap.sequence_number == 0) {
        // No snapshot has ever been published — idle, not yet live.
        live = false;
    } else if (!ever_observed_ || snap.sequence_number != last_sequence_) {
        live = true;
        ever_observed_ = true;
    } else {
        live = false;  // sequence did not advance → stale.
    }
    last_sequence_ = snap.sequence_number;

    // Build merged stats: probe-owned signal counters from the snapshot,
    // device counters mirrored from the host (never shadowed by the probe).
    audio::AudioStats stats = device_stats_;
    stats.callbacks = snap.callbacks;

    // Copy the most-recent captured waveform (non-RT read). Scope mode drains
    // multichannel history so acquisition and measurement share the same core
    // helpers as CLI/MCP; Signal keeps the legacy channel-0 diagnostic trace.
    int frames = 0;
    if (live) {
        if (mode_ == AudioInspectorMode::kScope) {
            const auto channels = std::max<std::size_t>(1, snap.channel_count);
            scope_capture_.resize(channels, AudioWaveformView::kCapacity);
            frames = probe_->read_capture(scope_capture_.view(),
                                          AudioWaveformView::kCapacity);
            if (frames > 0) {
                const auto ch0 = scope_capture_.channel(0);
                std::copy_n(ch0.data(),
                            std::min<int>(frames, AudioWaveformView::kCapacity),
                            waveform_scratch_.data());
            }
        } else {
            frames = probe_->read_capture(
                waveform_scratch_.data(),
                static_cast<int>(waveform_scratch_.size()));
        }
    }

    const auto status = live ? AudioInspectorPanel::Status::kLive
                             : AudioInspectorPanel::Status::kStale;
    panel_->update(status, snap, stats,
                   frames > 0 ? waveform_scratch_.data() : nullptr, frames);
    if (mode_ == AudioInspectorMode::kScope && live && frames > 0) {
        panel_->set_scope_result(build_scope_result(snap, scope_capture_, frames));
    } else {
        panel_->clear_scope_result();
    }
}

void AudioInspectorWindow::set_mode(AudioInspectorMode mode) {
    apply_mode(mode, true);
}

void AudioInspectorWindow::load_mode_preference() {
    if (!preferences_) {
        apply_mode(AudioInspectorMode::kSignal, false);
        return;
    }
    auto stored = preferences_->get_string(kAudioInspectorModePreference);
    apply_mode(stored ? parse_mode_pref(*stored) : AudioInspectorMode::kSignal,
               false);
}

void AudioInspectorWindow::save_mode_preference() {
    if (!preferences_) return;
    preferences_->set_string(kAudioInspectorModePreference, mode_pref_value(mode_));
    if (preferences_->path().empty()) return;
    (void)preferences_->save();
}

void AudioInspectorWindow::apply_mode(AudioInspectorMode mode, bool persist) {
    mode_ = mode;
    if (panel_ && panel_->mode() != mode_) {
        applying_mode_ = true;
        panel_->set_mode(mode_);
        applying_mode_ = false;
    }
    if (persist)
        save_mode_preference();
}

audio::AudioScopeResult AudioInspectorWindow::build_scope_result(
    const audio::AudioProbeSnapshot& snap,
    audio::Buffer<float>& capture,
    int frames) const {
    if (frames >= 0 && static_cast<std::size_t>(frames) < capture.num_samples())
        capture.resize(capture.num_channels(), static_cast<std::size_t>(frames));

    audio::AudioScopeResult result;
    result.stage = snap.stage_id;
    result.trigger_mode = audio::AudioScopeTriggerMode::kRisingZero;

    audio::AudioScopeAcquisitionConfig config;
    config.window_samples = static_cast<std::uint32_t>(
        std::max(0, std::min(frames, AudioWaveformView::kCapacity)));
    config.trigger_mode = result.trigger_mode;
    config.selected_channel = 0;
    const auto& const_capture = capture;
    result.acquisition = audio::acquire_audio_scope_window(
        const_capture.view(), config, &snap);
    result.measurements = audio::measure_audio_scope_window(result.acquisition);
    return result;
}

void AudioInspectorWindow::show() {
    if (window_host_ && window_host_->is_visible()) return;

    poll();

    if (!window_host_) {
        WindowOptions opts;
        opts.title = "Audio Inspector";
        opts.width = 300;
        opts.height = 460;
        opts.resizable = true;
        // Tool-window close policy: closing the audio inspector must never stop
        // the app. Mirrors InspectorWindow — secondary_window drives the macOS
        // close delegate to skip the app-stop, and we never set
        // initially_hidden so the hidden-close path is unreachable.
        opts.secondary_window = true;

        WindowType type = WindowType::inspector;
        opts.window_type = &type;

        if (parent_host_)
            opts.parent_native_handle = parent_host_->native_window_handle();

        window_host_ = host_factory_
            ? host_factory_(*root_, opts)
            : WindowHost::create(*root_, opts);
        if (!window_host_) {
            runtime::log_warn(
                "AudioInspectorWindow: WindowHost::create returned nullptr; "
                "audio inspector windows require a supported WindowHost "
                "implementation");
            return;
        }

        if (manager_) {
            window_id_ = manager_->register_window(
                window_host_.get(), root_.get(), WindowType::inspector);
        }
    }

    window_host_->show();
}

void AudioInspectorWindow::hide() {
    if (window_host_) window_host_->hide();
}

void AudioInspectorWindow::toggle() {
    if (is_visible())
        hide();
    else
        show();
}

bool AudioInspectorWindow::is_visible() const {
    return window_host_ && window_host_->is_visible();
}

void AudioInspectorWindow::register_command_handler(CommandRegistry& registry) {
    if (registry_ == &registry) return;
    if (registry_) registry_->remove_handler(this);
    registry_ = &registry;

    CommandInfo info;
    info.id = kToggleAudioInspector;
    info.name = "Toggle Audio Inspector";
    info.category = "Developer";
    info.default_key = KeyCode::a;
    // Cmd+Shift+A on macOS, Ctrl+Shift+A elsewhere — distinct from the layout
    // inspector's Cmd/Ctrl+I so both tools coexist. register_command() respects
    // a user rebind already loaded into the ShortcutMap.
#ifdef __APPLE__
    info.default_modifiers = kModCmd | kModShift;
#else
    info.default_modifiers = kModCtrl | kModShift;
#endif
    registry.register_command(info);
    registry.add_handler(this);
}

std::vector<CommandID> AudioInspectorWindow::commands() const {
    return {kToggleAudioInspector};
}

bool AudioInspectorWindow::perform_command(CommandID id) {
    if (id != kToggleAudioInspector) return false;
    toggle();
    return true;
}

}  // namespace pulp::view
