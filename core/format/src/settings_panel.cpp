#include <pulp/format/settings_panel.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <pulp/view/buttons.hpp>  // TextButton (momentary Done)
#include <algorithm>
#include <cmath>
#include <sstream>

namespace pulp::format {

namespace {

constexpr const char* kInputNotUsedText = "(Not used by this instrument)";

bool rate_matches(double a, double b) {
    return std::abs(a - b) < 1.0;
}

void constrain_rates(std::vector<double>& rates, const std::vector<double>& allowed) {
    if (allowed.empty()) return;

    std::vector<double> filtered;
    for (double rate : rates) {
        for (double allowed_rate : allowed) {
            if (rate_matches(rate, allowed_rate)) {
                filtered.push_back(allowed_rate);
                break;
            }
        }
    }
    if (filtered.empty()) filtered = allowed;
    std::sort(filtered.begin(), filtered.end());
    filtered.erase(std::unique(filtered.begin(), filtered.end(),
                               [](double a, double b) { return rate_matches(a, b); }),
                   filtered.end());
    rates = std::move(filtered);
}

void constrain_buffers(std::vector<int>& buffers, const std::vector<int>& allowed) {
    if (allowed.empty()) return;

    std::vector<int> filtered;
    for (int buffer : buffers) {
        if (std::find(allowed.begin(), allowed.end(), buffer) != allowed.end())
            filtered.push_back(buffer);
    }
    if (filtered.empty()) filtered = allowed;
    std::sort(filtered.begin(), filtered.end());
    filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());
    buffers = std::move(filtered);
}

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

void update_meter_from_bridge(view::AudioBridge* bridge, view::MultiMeter* meter) {
    if (!bridge || !meter) return;

    view::MeterData data;
    if (!bridge->pop_latest_meter(data)) return;

    auto converted = to_multi_channel_meter(data);
    meter->set_channel_count(converted.num_channels);
    meter->update(converted, 1.0f / 30.0f);
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
    out_combo->on_change = [this](int) {
        rebuild_rate_and_buffer_lists();
        apply_config();
    };
    audio_tab->add_child(std::move(out_combo));

    // Input device
    auto input_label = make_section_label("Input Device");
    input_device_label_ = input_label.get();
    audio_tab->add_child(std::move(input_label));
    auto in_combo = std::make_unique<view::ComboBox>();
    input_device_combo_ = in_combo.get();
    in_combo->flex().preferred_height = 28.0f;
    in_combo->on_change = [this](int) { apply_config(); };
    audio_tab->add_child(std::move(in_combo));

    // Sample rate
    audio_tab->add_child(make_section_label("Sample Rate"));
    auto rate_combo = std::make_unique<view::ComboBox>();
    sample_rate_combo_ = rate_combo.get();
    rate_combo->flex().preferred_height = 28.0f;
    rate_combo->on_change = [this](int) {
        update_latency_label();
        apply_config();
    };
    audio_tab->add_child(std::move(rate_combo));

    // Buffer size
    audio_tab->add_child(make_section_label("Buffer Size"));
    auto buf_combo = std::make_unique<view::ComboBox>();
    buffer_size_combo_ = buf_combo.get();
    buf_combo->flex().preferred_height = 28.0f;
    buf_combo->on_change = [this](int) {
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
                cfg.type = TestSignalType::sine;
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
        if (idx >= 0 && idx < static_cast<int>(midi_ports_.size())) {
            current_config_.midi_input_id = midi_ports_[static_cast<size_t>(idx)].id;
            apply_config();
        }
    };
    midi_tab->add_child(std::move(list));

    auto info = make_info_label("MIDI devices are detected automatically when plugged in.");
    midi_tab->add_child(std::move(info));

    tab_panel_->add_tab("MIDI", std::move(midi_tab));
}

void SettingsPanel::add_section(std::string title, std::unique_ptr<view::View> view) {
    if (tab_panel_ && view) tab_panel_->add_tab(std::move(title), std::move(view));
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

    rebuild_device_lists();
    rebuild_rate_and_buffer_lists();
    rebuild_midi_list();
    update_latency_label();
}

void SettingsPanel::set_current_config(const StandaloneConfig& cfg) {
    current_config_ = cfg;

    if (audio_sys_) rebuild_device_lists();

    int output_index = current_output_device_index();
    if (output_index >= 0 && output_device_combo_)
        output_device_combo_->set_selected_silent(output_index);

    rebuild_rate_and_buffer_lists();

    int rate_index = current_sample_rate_index();
    if (rate_index >= 0 && sample_rate_combo_)
        sample_rate_combo_->set_selected_silent(rate_index);

    int buffer_index = current_buffer_size_index();
    if (buffer_index >= 0 && buffer_size_combo_)
        buffer_size_combo_->set_selected_silent(buffer_index);

    for (size_t i = 0; i < midi_ports_.size(); ++i) {
        if (midi_ports_[i].id == cfg.midi_input_id) {
            if (midi_list_) midi_list_->set_selected(static_cast<int>(i));
            break;
        }
    }

    update_latency_label();
}

void SettingsPanel::rebuild_device_lists() {
    if (!audio_sys_) return;

    auto all_devices = audio_sys_->enumerate_devices();
    output_devices_.clear();
    input_devices_.clear();

    for (auto& d : all_devices) {
        if (d.max_output_channels > 0) output_devices_.push_back(d);
        if (d.max_input_channels > 0) input_devices_.push_back(d);
    }

    if (output_device_combo_) {
        // Index 0 is a synthetic "System Default (follow)" entry — selecting it keeps
        // audio_device_id empty so the app TRACKS the system default output live
        // (switch to AirPods/headphones and audio moves without relaunch). The real
        // devices follow, one-based.
        std::vector<std::string> names;
        names.push_back("System Default (follow)");
        for (auto& d : output_devices_)
            names.push_back(d.name + (d.is_default_output ? " (Default)" : ""));
        output_device_combo_->set_items(std::move(names));
        int output_index = current_output_device_index();
        if (output_index >= 0)
            output_device_combo_->set_selected_silent(output_index);
    }

    if (input_device_combo_) {
        std::vector<std::string> names;
        if (current_config_.supports_audio_input) {
            if (input_device_label_) input_device_label_->set_text("Input Device");
            names.push_back("(None)");
            for (auto& d : input_devices_)
                names.push_back(d.name + (d.is_default_input ? " (Default)" : ""));
        } else {
            if (input_device_label_) input_device_label_->set_text("Input Device");
            names.push_back(kInputNotUsedText);
        }
        input_device_combo_->set_items(std::move(names));
        int input_index = 0;
        if (current_config_.supports_audio_input && current_config_.input_channels > 0) {
            for (size_t i = 0; i < input_devices_.size(); ++i) {
                if (input_devices_[i].is_default_input) {
                    input_index = static_cast<int>(i + 1);
                    break;
                }
            }
            if (input_index == 0 && !input_devices_.empty()) input_index = 1;
        }
        input_device_combo_->set_selected_silent(input_index);
    }
}

void SettingsPanel::rebuild_midi_list() {
    if (!midi_sys_ || !midi_list_) return;

    midi_ports_ = midi_sys_->enumerate_inputs();

    std::vector<std::string> names;
    for (auto& p : midi_ports_) names.push_back(p.name);
    midi_list_->set_items(std::move(names));

    if (!midi_ports_.empty()) midi_list_->set_selected(0);
}

void SettingsPanel::rebuild_rate_and_buffer_lists() {
    available_rates_.clear();
    available_buffers_.clear();

    // Combo index 0 is "System Default (follow)" — real devices are one-based, so a
    // selection of 0 (follow) uses the generic fallback rates/buffers below (the
    // actual device follows live and DefaultOutput sample-rate-converts).
    int sel = output_device_combo_ ? (output_device_combo_->selected() - 1) : -1;
    if (sel >= 0 && sel < static_cast<int>(output_devices_.size())) {
        auto& dev = output_devices_[static_cast<size_t>(sel)];
        available_rates_ = dev.sample_rates;
        available_buffers_ = dev.buffer_sizes;
    }

    if (available_rates_.empty())
        available_rates_ = { 44100, 48000, 88200, 96000, 176400, 192000 };
    if (available_buffers_.empty())
        available_buffers_ = { 64, 128, 256, 512, 1024, 2048, 4096 };
    constrain_rates(available_rates_, current_config_.allowed_sample_rates);
    constrain_buffers(available_buffers_, current_config_.allowed_buffer_sizes);

    if (sample_rate_combo_) {
        std::vector<std::string> items;
        for (double r : available_rates_) {
            std::ostringstream ss;
            ss << static_cast<int>(r) << " Hz";
            items.push_back(ss.str());
        }
        sample_rate_combo_->set_items(std::move(items));
        int rate_index = current_sample_rate_index();
        if (rate_index >= 0) sample_rate_combo_->set_selected_silent(rate_index);
    }

    if (buffer_size_combo_) {
        std::vector<std::string> items;
        for (int b : available_buffers_) {
            double latency_ms = 0;
            if (!available_rates_.empty()) {
                int rate_idx = sample_rate_combo_ ? sample_rate_combo_->selected() : 0;
                if (rate_idx >= 0 && rate_idx < static_cast<int>(available_rates_.size()))
                    latency_ms = 1000.0 * b / available_rates_[static_cast<size_t>(rate_idx)];
            }
            std::ostringstream ss;
            ss << b << " samples";
            if (latency_ms > 0) {
                ss << " (";
                ss.precision(1);
                ss << std::fixed << latency_ms << " ms)";
            }
            items.push_back(ss.str());
        }
        buffer_size_combo_->set_items(std::move(items));
        int buffer_index = current_buffer_size_index();
        if (buffer_index >= 0) buffer_size_combo_->set_selected_silent(buffer_index);
    }
}

int SettingsPanel::current_output_device_index() const {
    // Index 0 = "System Default (follow)"; real devices are one-based.
    if (current_config_.audio_device_id.empty()) return 0;  // following the default
    for (size_t i = 0; i < output_devices_.size(); ++i) {
        if (output_devices_[i].id == current_config_.audio_device_id)
            return static_cast<int>(i) + 1;
    }
    return 0;  // a pinned device that's gone -> fall back to following the default
}

int SettingsPanel::current_sample_rate_index() const {
    if (available_rates_.empty()) return -1;

    for (size_t i = 0; i < available_rates_.size(); ++i) {
        if (rate_matches(available_rates_[i], current_config_.sample_rate))
            return static_cast<int>(i);
    }

    return 0;
}

int SettingsPanel::current_buffer_size_index() const {
    if (available_buffers_.empty()) return -1;

    for (size_t i = 0; i < available_buffers_.size(); ++i) {
        if (available_buffers_[i] == current_config_.buffer_size)
            return static_cast<int>(i);
    }

    return 0;
}

void SettingsPanel::update_latency_label() {
    if (!latency_label_) return;

    int rate_idx = sample_rate_combo_ ? sample_rate_combo_->selected() : -1;
    int buf_idx = buffer_size_combo_ ? buffer_size_combo_->selected() : -1;

    if (rate_idx >= 0 && rate_idx < static_cast<int>(available_rates_.size()) &&
        buf_idx >= 0 && buf_idx < static_cast<int>(available_buffers_.size())) {
        double rate = available_rates_[static_cast<size_t>(rate_idx)];
        int buf = available_buffers_[static_cast<size_t>(buf_idx)];
        double ms = 1000.0 * buf / rate;
        std::ostringstream ss;
        ss << "Latency: " << std::fixed;
        ss.precision(1);
        ss << ms << " ms";
        latency_label_->set_text(ss.str());
    } else {
        latency_label_->set_text("Latency: —");
    }
}

void SettingsPanel::apply_config() {
    if (!callbacks_.on_config_apply) return;

    StandaloneConfig cfg = current_config_;

    // Index 0 = "System Default (follow)" -> empty id (live-follow); real devices
    // are one-based and pin.
    int out_idx = output_device_combo_ ? output_device_combo_->selected() : -1;
    if (out_idx <= 0)
        cfg.audio_device_id = "";
    else if ((out_idx - 1) < static_cast<int>(output_devices_.size()))
        cfg.audio_device_id = output_devices_[static_cast<size_t>(out_idx - 1)].id;

    int in_idx = input_device_combo_ ? input_device_combo_->selected() : 0;
    if (!cfg.supports_audio_input) {
        cfg.input_channels = 0;
    } else if (in_idx > 0 && (in_idx - 1) < static_cast<int>(input_devices_.size())) {
        cfg.input_channels = std::min(2, input_devices_[static_cast<size_t>(in_idx - 1)].max_input_channels);
    } else {
        cfg.input_channels = 0;
    }

    int rate_idx = sample_rate_combo_ ? sample_rate_combo_->selected() : -1;
    if (rate_idx >= 0 && rate_idx < static_cast<int>(available_rates_.size()))
        cfg.sample_rate = available_rates_[static_cast<size_t>(rate_idx)];

    int buf_idx = buffer_size_combo_ ? buffer_size_combo_->selected() : -1;
    if (buf_idx >= 0 && buf_idx < static_cast<int>(available_buffers_.size()))
        cfg.buffer_size = available_buffers_[static_cast<size_t>(buf_idx)];

    int midi_idx = midi_list_ ? midi_list_->selected() : -1;
    if (midi_idx >= 0 && midi_idx < static_cast<int>(midi_ports_.size()))
        cfg.midi_input_id = midi_ports_[static_cast<size_t>(midi_idx)].id;

    current_config_ = cfg;
    callbacks_.on_config_apply(cfg);
}

void SettingsPanel::poll() {
    if (devices_changed_.exchange(false, std::memory_order_relaxed)) {
        rebuild_device_lists();
        rebuild_rate_and_buffer_lists();
        update_latency_label();
    }
    if (midi_changed_.exchange(false, std::memory_order_relaxed))
        rebuild_midi_list();

    update_meter_from_bridge(input_bridge_, input_meter_);
    update_meter_from_bridge(output_bridge_, output_meter_);
}

} // namespace pulp::format
