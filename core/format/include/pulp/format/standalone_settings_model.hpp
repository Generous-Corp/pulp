#pragma once

/// @file standalone_settings_model.hpp
/// The Audio/MIDI settings a standalone app offers, as a view-free model.
///
/// `StandaloneSettingsModel` turns a `StandaloneConfig` plus the audio and MIDI
/// systems' device lists into the rows a settings UI shows (output device,
/// input device, sample rate, buffer size, MIDI input, output latency) and
/// folds the user's choices back into a `StandaloneConfig`. It draws nothing and
/// owns no widget, so the Pulp view `SettingsPanel` and the native settings
/// window of a standalone without a view layer are thin shells over the same
/// rules: what a list offers, which rows are hidden, how a selection maps to a
/// config.
///
/// Device lists always come from the systems handed to `refresh()`. Nothing is
/// assumed: with no audio system the device lists hold only the follow-default
/// row, and the null device (`PULP_AUDIO_DEVICE=null`) appears because the null
/// audio system enumerates it.

#include <pulp/audio/device.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/midi/device.hpp>

#include <string>
#include <vector>

namespace pulp::format {

/// `rates` limited to `allowed` (matched within 1 Hz), sorted and de-duplicated.
/// Empty `allowed` returns `rates` unchanged; when nothing in `rates` is allowed
/// the result is `allowed` itself, so the list is never empty by constraint.
std::vector<double> constrain_sample_rates(std::vector<double> rates,
                                           const std::vector<double>& allowed);

/// The same for buffer sizes.
std::vector<int> constrain_buffer_sizes(std::vector<int> buffers, const std::vector<int>& allowed);

/// Latency, in milliseconds, of one buffer of `buffer_size` frames at
/// `sample_rate`. Zero when either is not positive.
double buffer_latency_ms(double sample_rate, int buffer_size) noexcept;

class StandaloneSettingsModel {
  public:
    struct Options {
        /// Offer a leading "None" row in the MIDI list (selecting it opens no
        /// MIDI input). Off: the list holds only ports.
        bool midi_includes_none = true;
    };

    StandaloneSettingsModel() = default;
    explicit StandaloneSettingsModel(Options options) : options_(options) {}

    /// Replace the config the selections resolve against, keeping the device
    /// lists as last enumerated.
    void set_config(const StandaloneConfig& config);

    /// Enumerate the devices of `audio` and the inputs of `midi` (either may be
    /// null) and re-resolve every selection against the current config.
    void refresh(audio::AudioSystem* audio, midi::MidiSystem* midi);

    // -- Rows -----------------------------------------------------------------

    /// Item 0 is "System Default (follow)", an empty `audio_device_id`; real
    /// output devices follow.
    const std::vector<std::string>& output_items() const {
        return output_items_;
    }
    /// With `supports_audio_input`: "(None)" then the input devices. Without:
    /// a single "not used" row, and `input_visible()` is false.
    const std::vector<std::string>& input_items() const {
        return input_items_;
    }
    /// Hidden when the processor has no audio input.
    bool input_visible() const {
        return config_.supports_audio_input;
    }
    const std::vector<std::string>& sample_rate_items() const {
        return rate_items_;
    }
    /// Each item carries the latency at the selected sample rate.
    std::vector<std::string> buffer_size_items() const;
    const std::vector<std::string>& midi_items() const {
        return midi_items_;
    }

    int output_selected() const {
        return output_selected_;
    }
    int input_selected() const {
        return input_selected_;
    }
    int sample_rate_selected() const {
        return rate_selected_;
    }
    int buffer_size_selected() const {
        return buffer_selected_;
    }
    int midi_selected() const {
        return midi_selected_;
    }

    /// The sample rates / buffer sizes the lists offer, after the device's own
    /// support and `StandaloneConfig::allowed_*` are applied.
    const std::vector<double>& sample_rates() const {
        return rates_;
    }
    const std::vector<int>& buffer_sizes() const {
        return buffers_;
    }

    // -- Selection ------------------------------------------------------------

    /// Choosing an output device rebuilds the rate and buffer lists, keeping the
    /// current values when the new lists still offer them.
    void select_output(int index);
    void select_input(int index);
    void select_sample_rate(int index);
    void select_buffer_size(int index);
    void select_midi(int index);

    // -- Output ---------------------------------------------------------------

    /// Latency of the selected buffer at the selected rate, in ms; zero when
    /// either selection is empty.
    double latency_ms() const;
    /// "Latency: 5.3 ms", or a placeholder in place of the number when unknown.
    std::string latency_text() const;

    /// The config the selections describe: the base config with the chosen
    /// device, input channel count, rate, buffer size and MIDI input applied.
    /// The null device is never written as a device id (it is a session choice,
    /// not a saved one); choosing it leaves the id empty.
    StandaloneConfig config() const;

    /// The id a "None" MIDI selection writes to `StandaloneConfig::midi_input_id`.
    static constexpr const char* kNoMidiInputId = StandaloneConfig::kNoMidiInput;

  private:
    void resolve();
    void rebuild_rates_and_buffers();
    void resolve_rate_and_buffer_selection();

    Options options_{};
    StandaloneConfig config_{};
    std::vector<audio::DeviceInfo> outputs_;
    std::vector<audio::DeviceInfo> inputs_;
    std::vector<midi::MidiPortInfo> midi_ports_;
    std::vector<double> rates_;
    std::vector<int> buffers_;
    std::vector<std::string> output_items_{"System Default (follow)"};
    std::vector<std::string> input_items_;
    std::vector<std::string> rate_items_;
    std::vector<std::string> midi_items_;
    int output_selected_ = 0;
    int input_selected_ = 0;
    int rate_selected_ = -1;
    int buffer_selected_ = -1;
    int midi_selected_ = -1;
};

} // namespace pulp::format
