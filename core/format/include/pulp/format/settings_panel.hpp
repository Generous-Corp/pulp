#pragma once

#include <pulp/audio/device.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/test_signal.hpp>
#include <pulp/midi/device.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace pulp::format {

/// Callbacks from SettingsPanel to the standalone host.
struct SettingsPanelCallbacks {
    std::function<void(const StandaloneConfig&)> on_config_apply;
    std::function<void(const TestSignalConfig&)> on_test_signal_changed;
    std::function<void(const std::string& path)> on_file_load;
    std::function<void(bool play, bool loop)> on_file_transport;
};

/// Audio/MIDI Settings panel with two tabs.
/// Audio tab: device selectors, sample rate, buffer size, input meters, test signal.
/// MIDI tab: port list with hotplug detection.
class SettingsPanel : public view::View {
public:
    SettingsPanel();

    void set_callbacks(SettingsPanelCallbacks cb) { callbacks_ = std::move(cb); }

    /// Bind audio/MIDI systems for enumeration and hotplug listening.
    void bind_systems(audio::AudioSystem* audio_sys, midi::MidiSystem* midi_sys);

    /// Set the current running config (to pre-select current values).
    void set_current_config(const StandaloneConfig& cfg);

    /// Provide an AudioBridge for input level metering.
    void set_input_meter_bridge(view::AudioBridge* bridge) { input_bridge_ = bridge; }

    /// Call periodically (~30 Hz) from idle/timer to refresh meters and hotplug.
    void poll();

private:
    void build_audio_tab();
    void build_midi_tab();
    void rebuild_device_lists();
    void rebuild_midi_list();
    void rebuild_rate_and_buffer_lists();
    void apply_config();
    void update_latency_label();

    SettingsPanelCallbacks callbacks_;
    audio::AudioSystem* audio_sys_ = nullptr;
    midi::MidiSystem* midi_sys_ = nullptr;
    view::AudioBridge* input_bridge_ = nullptr;

    StandaloneConfig current_config_;

    // Tab panel
    view::TabPanel* tab_panel_ = nullptr;

    // Audio tab widgets
    view::ComboBox* output_device_combo_ = nullptr;
    view::ComboBox* input_device_combo_ = nullptr;
    view::ComboBox* sample_rate_combo_ = nullptr;
    view::ComboBox* buffer_size_combo_ = nullptr;
    view::Label* latency_label_ = nullptr;
    view::MultiMeter* input_meter_ = nullptr;
    view::Toggle* test_tone_toggle_ = nullptr;
    view::ComboBox* tone_freq_combo_ = nullptr;

    // MIDI tab widgets
    view::ListBox* midi_list_ = nullptr;

    // Cached device info for mapping combo indices to IDs
    std::vector<audio::DeviceInfo> output_devices_;
    std::vector<audio::DeviceInfo> input_devices_;
    std::vector<midi::MidiPortInfo> midi_ports_;
    std::vector<double> available_rates_;
    std::vector<int> available_buffers_;

    // Hotplug flags (set from OS thread, read from UI poll)
    std::atomic<bool> devices_changed_{false};
    std::atomic<bool> midi_changed_{false};
};

} // namespace pulp::format
