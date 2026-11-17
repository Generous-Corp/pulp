#include <pulp/format/standalone_settings_model.hpp>

#include <pulp/format/detail/null_audio_device.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace pulp::format {

namespace {

constexpr const char* kFollowDefaultText = "System Default (follow)";
constexpr const char* kInputNoneText = "(None)";
constexpr const char* kInputNotUsedText = "(Not used by this instrument)";
constexpr const char* kMidiNoneText = "None";

bool rate_matches(double a, double b) {
    return std::abs(a - b) < 1.0;
}

bool is_null_device(const std::string& id) {
    return id == detail::kNullAudioDeviceId;
}

int clamp_index(int index, int count) {
    if (count <= 0)
        return -1;
    return std::clamp(index, 0, count - 1);
}

} // namespace

std::vector<double> constrain_sample_rates(std::vector<double> rates,
                                           const std::vector<double>& allowed) {
    if (allowed.empty())
        return rates;
    std::vector<double> filtered;
    for (double rate : rates) {
        for (double allowed_rate : allowed) {
            if (rate_matches(rate, allowed_rate)) {
                filtered.push_back(allowed_rate);
                break;
            }
        }
    }
    if (filtered.empty())
        filtered = allowed;
    std::sort(filtered.begin(), filtered.end());
    filtered.erase(std::unique(filtered.begin(), filtered.end(), rate_matches), filtered.end());
    return filtered;
}

std::vector<int> constrain_buffer_sizes(std::vector<int> buffers, const std::vector<int>& allowed) {
    if (allowed.empty())
        return buffers;
    std::vector<int> filtered;
    for (int buffer : buffers) {
        if (std::find(allowed.begin(), allowed.end(), buffer) != allowed.end())
            filtered.push_back(buffer);
    }
    if (filtered.empty())
        filtered = allowed;
    std::sort(filtered.begin(), filtered.end());
    filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());
    return filtered;
}

double buffer_latency_ms(double sample_rate, int buffer_size) noexcept {
    if (sample_rate <= 0.0 || buffer_size <= 0)
        return 0.0;
    return 1000.0 * static_cast<double>(buffer_size) / sample_rate;
}

void StandaloneSettingsModel::set_config(const StandaloneConfig& config) {
    config_ = config;
    resolve();
}

void StandaloneSettingsModel::refresh(audio::AudioSystem* audio, midi::MidiSystem* midi) {
    outputs_.clear();
    inputs_.clear();
    midi_ports_.clear();
    if (audio) {
        for (auto& d : audio->enumerate_devices()) {
            if (d.max_output_channels > 0)
                outputs_.push_back(d);
            if (d.max_input_channels > 0)
                inputs_.push_back(std::move(d));
        }
    }
    if (midi)
        midi_ports_ = midi->enumerate_inputs();
    resolve();
}

void StandaloneSettingsModel::resolve() {
    // Output device: index 0 follows the system default.
    output_items_.assign(1, kFollowDefaultText);
    for (auto& d : outputs_)
        output_items_.push_back(d.name + (d.is_default_output ? " (Default)" : ""));
    output_selected_ = 0;
    if (!config_.audio_device_id.empty()) {
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (outputs_[i].id == config_.audio_device_id) {
                output_selected_ = static_cast<int>(i) + 1;
                break;
            }
        }
    }

    // Input device.
    input_items_.clear();
    input_selected_ = 0;
    if (config_.supports_audio_input) {
        input_items_.push_back(kInputNoneText);
        for (auto& d : inputs_)
            input_items_.push_back(d.name + (d.is_default_input ? " (Default)" : ""));
        if (config_.input_channels > 0) {
            for (size_t i = 0; i < inputs_.size(); ++i) {
                if (inputs_[i].is_default_input) {
                    input_selected_ = static_cast<int>(i) + 1;
                    break;
                }
            }
            if (input_selected_ == 0 && !inputs_.empty())
                input_selected_ = 1;
        }
    } else {
        input_items_.push_back(kInputNotUsedText);
    }

    rebuild_rates_and_buffers();

    // MIDI input. An empty id opens the first port, so it selects that row.
    midi_items_.clear();
    const int offset = options_.midi_includes_none ? 1 : 0;
    if (options_.midi_includes_none)
        midi_items_.push_back(kMidiNoneText);
    for (auto& p : midi_ports_)
        midi_items_.push_back(p.name);
    if (config_.midi_input_id == kNoMidiInputId) {
        midi_selected_ = options_.midi_includes_none ? 0 : -1;
    } else if (config_.midi_input_id.empty()) {
        midi_selected_ = midi_ports_.empty() ? (options_.midi_includes_none ? 0 : -1) : offset;
    } else {
        midi_selected_ = options_.midi_includes_none ? 0 : (midi_ports_.empty() ? -1 : 0);
        for (size_t i = 0; i < midi_ports_.size(); ++i) {
            if (midi_ports_[i].id == config_.midi_input_id) {
                midi_selected_ = static_cast<int>(i) + offset;
                break;
            }
        }
    }
}

void StandaloneSettingsModel::rebuild_rates_and_buffers() {
    rates_.clear();
    buffers_.clear();
    // Item 0 (follow the default) has no device to ask, so it offers the
    // generic lists; a pinned device offers what it reports.
    const int dev = output_selected_ - 1;
    if (dev >= 0 && dev < static_cast<int>(outputs_.size())) {
        rates_ = outputs_[static_cast<size_t>(dev)].sample_rates;
        buffers_ = outputs_[static_cast<size_t>(dev)].buffer_sizes;
    }
    if (rates_.empty())
        rates_ = {44100, 48000, 88200, 96000, 176400, 192000};
    if (buffers_.empty())
        buffers_ = {64, 128, 256, 512, 1024, 2048, 4096};
    rates_ = constrain_sample_rates(std::move(rates_), config_.allowed_sample_rates);
    buffers_ = constrain_buffer_sizes(std::move(buffers_), config_.allowed_buffer_sizes);

    rate_items_.clear();
    for (double r : rates_)
        rate_items_.push_back(std::to_string(static_cast<int>(r)) + " Hz");
    resolve_rate_and_buffer_selection();
}

void StandaloneSettingsModel::resolve_rate_and_buffer_selection() {
    rate_selected_ = rates_.empty() ? -1 : 0;
    for (size_t i = 0; i < rates_.size(); ++i) {
        if (rate_matches(rates_[i], config_.sample_rate)) {
            rate_selected_ = static_cast<int>(i);
            break;
        }
    }
    buffer_selected_ = buffers_.empty() ? -1 : 0;
    for (size_t i = 0; i < buffers_.size(); ++i) {
        if (buffers_[i] == config_.buffer_size) {
            buffer_selected_ = static_cast<int>(i);
            break;
        }
    }
}

std::vector<std::string> StandaloneSettingsModel::buffer_size_items() const {
    const double rate = (rate_selected_ >= 0 && rate_selected_ < static_cast<int>(rates_.size()))
                            ? rates_[static_cast<size_t>(rate_selected_)]
                            : 0.0;
    std::vector<std::string> items;
    for (int b : buffers_) {
        std::ostringstream ss;
        ss << b << " samples";
        const double ms = buffer_latency_ms(rate, b);
        if (ms > 0.0)
            ss << " (" << std::fixed << std::setprecision(1) << ms << " ms)";
        items.push_back(ss.str());
    }
    return items;
}

void StandaloneSettingsModel::select_output(int index) {
    index = clamp_index(index, static_cast<int>(output_items_.size()));
    if (index < 0)
        return;
    // Carry the user's current rate and buffer across the device change.
    if (rate_selected_ >= 0)
        config_.sample_rate = rates_[static_cast<size_t>(rate_selected_)];
    if (buffer_selected_ >= 0)
        config_.buffer_size = buffers_[static_cast<size_t>(buffer_selected_)];
    output_selected_ = index;
    config_.audio_device_id.clear();
    if (index > 0 && !is_null_device(outputs_[static_cast<size_t>(index - 1)].id))
        config_.audio_device_id = outputs_[static_cast<size_t>(index - 1)].id;
    rebuild_rates_and_buffers();
}

void StandaloneSettingsModel::select_input(int index) {
    if (!config_.supports_audio_input)
        return;
    index = clamp_index(index, static_cast<int>(input_items_.size()));
    if (index < 0)
        return;
    input_selected_ = index;
    config_.input_channels =
        index > 0 ? std::min(2, inputs_[static_cast<size_t>(index - 1)].max_input_channels) : 0;
}

void StandaloneSettingsModel::select_sample_rate(int index) {
    index = clamp_index(index, static_cast<int>(rates_.size()));
    if (index < 0)
        return;
    rate_selected_ = index;
    config_.sample_rate = rates_[static_cast<size_t>(index)];
}

void StandaloneSettingsModel::select_buffer_size(int index) {
    index = clamp_index(index, static_cast<int>(buffers_.size()));
    if (index < 0)
        return;
    buffer_selected_ = index;
    config_.buffer_size = buffers_[static_cast<size_t>(index)];
}

void StandaloneSettingsModel::select_midi(int index) {
    index = clamp_index(index, static_cast<int>(midi_items_.size()));
    if (index < 0)
        return;
    midi_selected_ = index;
    if (options_.midi_includes_none) {
        config_.midi_input_id = index == 0 ? std::string(kNoMidiInputId)
                                           : midi_ports_[static_cast<size_t>(index - 1)].id;
    } else {
        config_.midi_input_id = midi_ports_[static_cast<size_t>(index)].id;
    }
}

double StandaloneSettingsModel::latency_ms() const {
    if (rate_selected_ < 0 || buffer_selected_ < 0)
        return 0.0;
    return buffer_latency_ms(rates_[static_cast<size_t>(rate_selected_)],
                             buffers_[static_cast<size_t>(buffer_selected_)]);
}

std::string StandaloneSettingsModel::latency_text() const {
    const double ms = latency_ms();
    if (ms <= 0.0)
        return "Latency: \xe2\x80\x94";
    std::ostringstream ss;
    ss << "Latency: " << std::fixed << std::setprecision(1) << ms << " ms";
    return ss.str();
}

StandaloneConfig StandaloneSettingsModel::config() const {
    StandaloneConfig out = config_;
    if (!out.supports_audio_input)
        out.input_channels = 0;
    if (rate_selected_ >= 0)
        out.sample_rate = rates_[static_cast<size_t>(rate_selected_)];
    if (buffer_selected_ >= 0)
        out.buffer_size = buffers_[static_cast<size_t>(buffer_selected_)];
    return out;
}

} // namespace pulp::format
