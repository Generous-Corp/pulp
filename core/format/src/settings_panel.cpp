#include <algorithm>
#include <cmath>
#include <pulp/format/app_updates_settings_view.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/format/standalone_settings_model.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <pulp/view/buttons.hpp> // TextButton (momentary Done)
#include <sstream>

namespace pulp::format {

namespace {

signal::MultiChannelMeterData to_multi_channel_meter(const view::MeterData& data) {
    signal::MultiChannelMeterData out;
    out.num_channels = std::clamp(data.num_channels, 0, signal::kMaxMeterChannels);
    for (int ch = 0; ch < out.num_channels; ++ch) {
        out.channels[static_cast<size_t>(ch)].peak = data.peak[ch];
        out.channels[static_cast<size_t>(ch)].rms = data.rms[ch];
        out.channels[static_cast<size_t>(ch)].clipped = data.peak[ch] >= 1.0f;
    }
    return out;
}

void update_meter_from_bridge(view::AudioBridge* bridge, view::MultiMeter* meter,
                              bool request_repaint) {
    if (!bridge || !meter) return;

    view::MeterData data;
    if (!bridge->pop_latest_meter(data)) return;

    auto converted = to_multi_channel_meter(data);
    // set_channel_count() changes how many bars are drawn without touching a
    // level, so a device swap on a silent bus would otherwise leave the old bar
    // count on screen. Note it before the update consumes it.
    const bool channels_changed = converted.num_channels != meter->channel_count();
    meter->set_channel_count(converted.num_channels);
    // REPAINT ONLY WHEN THE NEEDLE MOVED. `pop_latest_meter` reads a triple
    // buffer and reports success for any published block, new or not, so this
    // runs on every host tick for as long as an audio device is open — through
    // silence, with the panel shut. An unconditional request_repaint() here
    // marks the WHOLE window dirty, which arms the host's needs_repaint flag
    // before every vsync and composites a full frame at the display's refresh
    // rate for the life of the process. Nothing moved, so ask for nothing.
    const bool levels_moved = meter->update(converted, 1.0f / 30.0f);
    if (request_repaint && (levels_moved || channels_changed))
        meter->request_repaint();
}

// ── Natural panel height ────────────────────────────────────────────────────
// Sum of the Audio tab's row layout (the tallest tab, shown first) so the host
// can size the window tall enough that nothing compresses. Keep in lockstep with
// build_audio_tab(): the same preferred heights, the 8px inter-row gap, and the
// 12px padding on all four sides.
constexpr int kRowGap = 8;       // audio_tab flex().gap
constexpr int kTabPadding = 12;  // audio_tab flex().padding (per side)
constexpr int kSectionLabel = 20;
constexpr int kInfoLabel = 16;
constexpr int kCombo = 28;
constexpr int kMeter = 48;
constexpr int kToggleRow = 28;  // test-signal header row

// 15 rows in build_audio_tab(): 6 section labels, 5 combos (output/input/rate/
// buffer device + the test-signal frequency combo), 1 info label (latency),
// 2 meters, and the test-signal header row.
constexpr int kAudioRowCount = 15;
constexpr int kAudioTabContent =
    6 * kSectionLabel + 5 * kCombo + 1 * kInfoLabel + 2 * kMeter + kToggleRow;
constexpr int kAudioTabNatural =
    kAudioTabContent + (kAudioRowCount - 1) * kRowGap + 2 * kTabPadding;  // 14 gaps

constexpr int kHeaderHeight = 52;     // panel header (Done bar)
constexpr int kInnerTabBarHeight = 32;  // SettingsPanel's inner Audio/MIDI tab bar

}  // namespace

int SettingsPanel::preferred_height() {
    return kHeaderHeight + kInnerTabBarHeight + kAudioTabNatural;
}

// ── Helper: create a styled label ──────────────────────────────────────────

static std::unique_ptr<view::Label> make_section_label(const std::string& text) {
    auto label = std::make_unique<view::Label>();
    label->set_text(text);
    label->flex().preferred_height = 20.0f;
    return label;
}

static std::unique_ptr<view::Label> make_info_label(const std::string& text) {
    auto label = std::make_unique<view::Label>();
    label->set_text(text);
    label->flex().preferred_height = 16.0f;
    return label;
}

// ── SettingsPanel ──────────────────────────────────────────────────────────

SettingsPanel::SettingsPanel() {
    flex().direction = view::FlexDirection::column;
    flex().flex_grow = 1.0f;

    // Header: a right-aligned "Done" (momentary button) that returns to the editor. The
    // standalone hosts this panel inside an outer card-stack TabPanel (tab bar hidden) whose
    // tab 0 is the editor — walk up to it and switch back. No-op if there's no such ancestor.
    // Keep this bar IDENTICAL to the editor's gear bar (height/padding) so the top-right
    // button doesn't shift vertically when switching between the editor and Settings.
    auto header = std::make_unique<view::View>();
    header->flex().direction = view::FlexDirection::row;
    header->flex().padding = 12.0f;
    header->flex().preferred_height = 52.0f;
    header->flex().flex_shrink = 0.0f;
    header->flex().align_items = view::FlexAlign::center;
    auto spacer = std::make_unique<view::View>();
    spacer->flex().flex_grow = 1.0f;
    header->add_child(std::move(spacer));
    auto done = std::make_unique<view::TextButton>("Done");
    done->flex().preferred_width = 112.0f;
    done->flex().preferred_height = 28.0f;
    done->on_click = [this] {
        for (view::View* v = parent(); v != nullptr; v = v->parent())
            if (auto* tp = dynamic_cast<view::TabPanel*>(v)) {
                tp->set_active_tab(0);
                return;
            }
    };
    header->add_child(std::move(done));
    add_child(std::move(header));

    auto tabs = std::make_unique<view::TabPanel>();
    tab_panel_ = tabs.get();
    tabs->flex().flex_grow = 1.0f;

    // Build tab contents before adding TabPanel to tree
    build_audio_tab();
    build_midi_tab();

    add_child(std::move(tabs));
}

void SettingsPanel::build_audio_tab() {
    auto audio_tab = std::make_unique<view::View>();
    audio_tab->flex().direction = view::FlexDirection::column;
    audio_tab->flex().flex_grow = 1.0f;
    audio_tab->flex().padding = 12.0f;
    audio_tab->flex().gap = 8.0f;

    // Output device
    audio_tab->add_child(make_section_label("Output Device"));
    auto out_combo = std::make_unique<view::ComboBox>();
    output_device_combo_ = out_combo.get();
    out_combo->flex().preferred_height = 28.0f;
    out_combo->on_change = [this](int index) {
        model_.select_output(index);
        sync_selectors();
        apply_config();
    };
    audio_tab->add_child(std::move(out_combo));

    // Input device
    auto input_label = make_section_label(StandaloneSettingsModel::input_label());
    input_device_label_ = input_label.get();
    audio_tab->add_child(std::move(input_label));
    auto in_combo = std::make_unique<view::ComboBox>();
    input_device_combo_ = in_combo.get();
    in_combo->flex().preferred_height = 28.0f;
    in_combo->on_change = [this](int index) {
        model_.select_input(index);
        apply_config();
    };
    audio_tab->add_child(std::move(in_combo));

    // Sample rate
    audio_tab->add_child(make_section_label("Sample Rate"));
    auto rate_combo = std::make_unique<view::ComboBox>();
    sample_rate_combo_ = rate_combo.get();
    rate_combo->flex().preferred_height = 28.0f;
    rate_combo->on_change = [this](int index) {
        model_.select_sample_rate(index);
        sync_buffer_items();
        update_latency_label();
        apply_config();
    };
    audio_tab->add_child(std::move(rate_combo));

    // Buffer size
    audio_tab->add_child(make_section_label("Buffer Size"));
    auto buf_combo = std::make_unique<view::ComboBox>();
    buffer_size_combo_ = buf_combo.get();
    buf_combo->flex().preferred_height = 28.0f;
    buf_combo->on_change = [this](int index) {
        model_.select_buffer_size(index);
        update_latency_label();
        apply_config();
    };
    audio_tab->add_child(std::move(buf_combo));

    // Latency display
    auto lat = make_info_label("Latency: —");
    latency_label_ = lat.get();
    audio_tab->add_child(std::move(lat));

    // Input level meter
    audio_tab->add_child(make_section_label("Input Level"));
    auto meter = std::make_unique<view::MultiMeter>();
    input_meter_ = meter.get();
    meter->set_channel_count(2);
    meter->set_layout(view::MultiMeter::Layout::horizontal);
    meter->set_display_style(view::MultiMeter::DisplayStyle::segmented);
    meter->flex().preferred_height = 48.0f;
    audio_tab->add_child(std::move(meter));

    audio_tab->add_child(make_section_label("Output Level"));
    auto out_meter = std::make_unique<view::MultiMeter>();
    output_meter_ = out_meter.get();
    out_meter->set_channel_count(2);
    out_meter->set_layout(view::MultiMeter::Layout::horizontal);
    out_meter->set_display_style(view::MultiMeter::DisplayStyle::segmented);
    out_meter->flex().preferred_height = 48.0f;
    audio_tab->add_child(std::move(out_meter));

    // Test tone section. Header line: "Test Signal" on the left, the "Sine Tone" switch on
    // the right; the frequency dropdown sits full-width on its own line below so it never
    // gets squished.
    auto ts_header = std::make_unique<view::View>();
    ts_header->flex().direction = view::FlexDirection::row;
    ts_header->flex().align_items = view::FlexAlign::center;
    ts_header->flex().gap = 8.0f;
    ts_header->flex().preferred_height = 28.0f;
    ts_header->add_child(make_section_label("Test Signal"));
    auto ts_spacer = std::make_unique<view::View>();
    ts_spacer->flex().flex_grow = 1.0f;
    ts_header->add_child(std::move(ts_spacer));
    auto tone_label = make_info_label("Sine Tone");
    tone_label->flex().flex_shrink = 0.0f;
    ts_header->add_child(std::move(tone_label));

    // Switch only (no stacked label) — compact and clearly clickable.
    auto tone_toggle = std::make_unique<view::Toggle>();
    test_tone_toggle_ = tone_toggle.get();
    tone_toggle->flex().preferred_width = 48.0f;
    tone_toggle->flex().preferred_height = 26.0f;
    tone_toggle->flex().flex_shrink = 0.0f;
    tone_toggle->on_toggle = [this](bool on) {
        if (callbacks_.on_test_signal_changed) {
            TestSignalConfig cfg;
            if (on) {
                // Signal type: 0 = Sine, 1 = Noise (broadband). Noise ignores the
                // frequency dropdown but exercises the whole spectrum, so the
                // analyzer shows the EQ shape carved into it.
                const int type_idx = signal_type_combo_ ? signal_type_combo_->selected() : 0;
                cfg.type = (type_idx == 1) ? TestSignalType::noise : TestSignalType::sine;
                static const float freqs[] = { 220.0f, 440.0f, 880.0f, 1000.0f };
                int idx = tone_freq_combo_ ? tone_freq_combo_->selected() : 1;
                if (idx >= 0 && idx < 4) cfg.sine_frequency_hz = freqs[idx];
                cfg.sine_amplitude = 0.5f;
            }
            callbacks_.on_test_signal_changed(cfg);
        }
    };
    ts_header->add_child(std::move(tone_toggle));
    audio_tab->add_child(std::move(ts_header));

    // Signal type: Sine tone or broadband Noise.
    auto type_combo = std::make_unique<view::ComboBox>();
    signal_type_combo_ = type_combo.get();
    type_combo->set_items({ "Sine", "Noise" });
    type_combo->set_selected_silent(0);
    type_combo->flex().preferred_height = 28.0f;
    type_combo->on_change = [this](int) {
        if (test_tone_toggle_ && test_tone_toggle_->is_on())
            test_tone_toggle_->on_toggle(true);   // re-emit with the new type
    };
    audio_tab->add_child(std::move(type_combo));

    auto freq_combo = std::make_unique<view::ComboBox>();
    tone_freq_combo_ = freq_combo.get();
    freq_combo->set_items({ "220 Hz (A3)", "440 Hz (A4)", "880 Hz (A5)", "1000 Hz" });
    freq_combo->set_selected_silent(1);
    freq_combo->flex().preferred_height = 28.0f;
    freq_combo->on_change = [this](int) {
        if (test_tone_toggle_ && test_tone_toggle_->is_on()) {
            test_tone_toggle_->on_toggle(true);
        }
    };
    audio_tab->add_child(std::move(freq_combo));

    tab_panel_->add_tab("Audio", std::move(audio_tab));
}

void SettingsPanel::build_midi_tab() {
    auto midi_tab = std::make_unique<view::View>();
    midi_tab->flex().direction = view::FlexDirection::column;
    midi_tab->flex().flex_grow = 1.0f;
    midi_tab->flex().padding = 12.0f;
    midi_tab->flex().gap = 8.0f;

    midi_tab->add_child(make_section_label("MIDI Input Ports"));

    auto list = std::make_unique<view::ListBox>();
    midi_list_ = list.get();
    list->flex().flex_grow = 1.0f;
    list->on_select = [this](int idx) {
        if (idx >= 0 && idx < static_cast<int>(model_.midi_items().size())) {
            model_.select_midi(idx);
            apply_config();
        }
    };
    midi_tab->add_child(std::move(list));

    auto info = make_info_label("MIDI devices are detected automatically when plugged in.");
    midi_tab->add_child(std::move(info));

    tab_panel_->add_tab("MIDI", std::move(midi_tab));
}

void SettingsPanel::add_section(std::string title, std::unique_ptr<view::View> view) {
    if (!tab_panel_ || !view)
        return;
    if (auto* updates = dynamic_cast<AppUpdatesSettingsView*>(view.get()))
        updates_view_ = updates;
    tab_panel_->add_tab(std::move(title), std::move(view));
}

int SettingsPanel::tab_count() const {
    return tab_panel_ ? tab_panel_->tab_count() : 0;
}

void SettingsPanel::bind_systems(audio::AudioSystem* audio_sys, midi::MidiSystem* midi_sys) {
    audio_sys_ = audio_sys;
    midi_sys_ = midi_sys;

    if (audio_sys_) {
        audio_sys_->set_device_change_callback([this] {
            devices_changed_.store(true, std::memory_order_relaxed);
        });
    }
    if (midi_sys_) {
        midi_sys_->set_port_change_callback([this] {
            midi_changed_.store(true, std::memory_order_relaxed);
        });
    }

    model_.refresh(audio_sys_, midi_sys_);
    sync_selectors();
}

void SettingsPanel::set_current_config(const StandaloneConfig& cfg) {
    model_.set_config(cfg);
    model_.refresh(audio_sys_, midi_sys_);
    sync_selectors();
}

void SettingsPanel::sync_selectors() {
    // Index 0 of the output list is "System Default (follow)", an empty
    // audio_device_id, so the app tracks the system default output live.
    if (output_device_combo_) {
        output_device_combo_->set_items(model_.output_items());
        output_device_combo_->set_selected_silent(model_.output_selected());
    }
    if (input_device_label_)
        input_device_label_->set_text(model_.input_label());
    if (input_device_combo_) {
        input_device_combo_->set_items(model_.input_items());
        input_device_combo_->set_selected_silent(model_.input_selected());
    }
    if (sample_rate_combo_) {
        sample_rate_combo_->set_items(model_.sample_rate_items());
        if (model_.sample_rate_selected() >= 0)
            sample_rate_combo_->set_selected_silent(model_.sample_rate_selected());
    }
    sync_buffer_items();
    sync_midi_list();
    update_latency_label();
}

void SettingsPanel::sync_buffer_items() {
    if (!buffer_size_combo_)
        return;
    buffer_size_combo_->set_items(model_.buffer_size_items());
    if (model_.buffer_size_selected() >= 0)
        buffer_size_combo_->set_selected_silent(model_.buffer_size_selected());
}

void SettingsPanel::sync_midi_list() {
    if (!midi_list_)
        return;
    midi_list_->set_items(model_.midi_items());
    if (model_.midi_selected() >= 0)
        midi_list_->set_selected(model_.midi_selected());
}

void SettingsPanel::update_latency_label() {
    if (latency_label_)
        latency_label_->set_text(model_.latency_text());
}

void SettingsPanel::apply_config() {
    if (!callbacks_.on_config_apply) return;
    callbacks_.on_config_apply(model_.config());
}

void SettingsPanel::poll() {
    const bool devices_changed = devices_changed_.exchange(false, std::memory_order_relaxed);
    const bool midi_changed = midi_changed_.exchange(false, std::memory_order_relaxed);
    if (devices_changed || midi_changed) {
        // Both systems can report one hotplug transaction before the next UI
        // tick. Enumerate them together so the model resolves one coherent
        // snapshot and each backend is queried only once per poll.
        model_.refresh(audio_sys_, midi_sys_);
        sync_selectors();
    }

    // Device and MIDI hotplug are event-driven and must stay current while the
    // panel is shut, so the lists above rebuild regardless. Meter ballistics
    // must advance too: freezing them while hidden reopens Settings on a stale
    // peak and restarts its hold/release clock. Hidden meters consume the
    // latest state but never ask the window for a frame.
    const bool repaint = visible();
    update_meter_from_bridge(input_bridge_, input_meter_, repaint);
    update_meter_from_bridge(output_bridge_, output_meter_, repaint);

    // A check finishes (and the last-check time moves) on Sparkle's schedule,
    // not on a click here, so the Updates tab re-reads its status about once
    // a second while Settings is open.
    if (updates_view_ && repaint && ++updates_poll_count_ >= 30) {
        updates_poll_count_ = 0;
        updates_view_->refresh();
    }
}

} // namespace pulp::format
