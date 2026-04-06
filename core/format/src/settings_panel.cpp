#include <pulp/format/settings_panel.hpp>
#include <pulp/runtime/log.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace pulp::format {

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
    audio_tab->add_child(make_section_label("Input Device"));
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
    meter->flex().preferred_height = 24.0f;
    audio_tab->add_child(std::move(meter));

    // Test tone section
    audio_tab->add_child(make_section_label("Test Signal"));

    auto tone_row = std::make_unique<view::View>();
    tone_row->flex().direction = view::FlexDirection::row;
    tone_row->flex().gap = 8.0f;
    tone_row->flex().preferred_height = 28.0f;

    auto tone_toggle = std::make_unique<view::Toggle>();
    test_tone_toggle_ = tone_toggle.get();
    tone_toggle->set_label("Sine Tone");
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
    tone_row->add_child(std::move(tone_toggle));

    auto freq_combo = std::make_unique<view::ComboBox>();
    tone_freq_combo_ = freq_combo.get();
    freq_combo->set_items({ "220 Hz (A3)", "440 Hz (A4)", "880 Hz (A5)", "1000 Hz" });
    freq_combo->set_selected_silent(1);
    freq_combo->flex().flex_grow = 1.0f;
    freq_combo->on_change = [this](int) {
        if (test_tone_toggle_ && test_tone_toggle_->is_on()) {
            test_tone_toggle_->on_toggle(true);
        }
    };
    tone_row->add_child(std::move(freq_combo));

    audio_tab->add_child(std::move(tone_row));

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
    rebuild_midi_list();
}

void SettingsPanel::set_current_config(const StandaloneConfig& cfg) {
    current_config_ = cfg;

    for (size_t i = 0; i < output_devices_.size(); ++i) {
        if (output_devices_[i].id == cfg.audio_device_id ||
            output_devices_[i].is_default_output) {
            if (output_device_combo_) output_device_combo_->set_selected_silent(static_cast<int>(i));
            break;
        }
    }

    rebuild_rate_and_buffer_lists();

    for (size_t i = 0; i < available_rates_.size(); ++i) {
        if (std::abs(available_rates_[i] - cfg.sample_rate) < 1.0) {
            if (sample_rate_combo_) sample_rate_combo_->set_selected_silent(static_cast<int>(i));
            break;
        }
    }

    for (size_t i = 0; i < available_buffers_.size(); ++i) {
        if (available_buffers_[i] == cfg.buffer_size) {
            if (buffer_size_combo_) buffer_size_combo_->set_selected_silent(static_cast<int>(i));
            break;
        }
    }

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
        std::vector<std::string> names;
        for (auto& d : output_devices_)
            names.push_back(d.name + (d.is_default_output ? " (Default)" : ""));
        output_device_combo_->set_items(std::move(names));
        if (!output_devices_.empty())
            output_device_combo_->set_selected_silent(0);
    }

    if (input_device_combo_) {
        std::vector<std::string> names;
        names.push_back("(None)");
        for (auto& d : input_devices_)
            names.push_back(d.name + (d.is_default_input ? " (Default)" : ""));
        input_device_combo_->set_items(std::move(names));
        input_device_combo_->set_selected_silent(0);
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

    int sel = output_device_combo_ ? output_device_combo_->selected() : -1;
    if (sel >= 0 && sel < static_cast<int>(output_devices_.size())) {
        auto& dev = output_devices_[static_cast<size_t>(sel)];
        available_rates_ = dev.sample_rates;
        available_buffers_ = dev.buffer_sizes;
    }

    if (available_rates_.empty())
        available_rates_ = { 44100, 48000, 88200, 96000, 176400, 192000 };
    if (available_buffers_.empty())
        available_buffers_ = { 64, 128, 256, 512, 1024, 2048, 4096 };

    if (sample_rate_combo_) {
        std::vector<std::string> items;
        for (double r : available_rates_) {
            std::ostringstream ss;
            ss << static_cast<int>(r) << " Hz";
            items.push_back(ss.str());
        }
        sample_rate_combo_->set_items(std::move(items));
        if (!available_rates_.empty()) sample_rate_combo_->set_selected_silent(0);
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
        if (!available_buffers_.empty()) buffer_size_combo_->set_selected_silent(0);
    }
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

    int out_idx = output_device_combo_ ? output_device_combo_->selected() : -1;
    if (out_idx >= 0 && out_idx < static_cast<int>(output_devices_.size()))
        cfg.audio_device_id = output_devices_[static_cast<size_t>(out_idx)].id;

    int in_idx = input_device_combo_ ? input_device_combo_->selected() : 0;
    if (in_idx > 0 && (in_idx - 1) < static_cast<int>(input_devices_.size()))
        cfg.input_channels = std::min(2, input_devices_[static_cast<size_t>(in_idx - 1)].max_input_channels);
    else
        cfg.input_channels = 0;

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
    if (devices_changed_.exchange(false, std::memory_order_relaxed))
        rebuild_device_lists();
    if (midi_changed_.exchange(false, std::memory_order_relaxed))
        rebuild_midi_list();

    // Update input meters
    if (input_bridge_ && input_meter_) {
        view::MeterData data;
        if (input_bridge_->pop_latest_meter(data)) {
            // Convert to MultiChannelMeterData for MultiMeter::update()
            // For now, just trigger a repaint — the meter draws from its own ballistics
            // A future enhancement could feed raw peak/rms into the meter
        }
    }
}

} // namespace pulp::format
