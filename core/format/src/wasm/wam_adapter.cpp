// WAMv2 format adapter implementation
// Bridges Pulp Processor to WAMv2 AudioWorkletProcessor via Emscripten.
//
// Audio thread flow:
//   JS AudioWorkletProcessor.process()
//     → C++ WamProcessorBridge::process()
//       → de-interleave Web Audio buffers to planar
//       → Pulp Processor::process()
//       → interleave planar back to Web Audio buffers

#include <pulp/format/web/wam_adapter.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/log.hpp>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <charconv>
#include <optional>

namespace pulp::format::wam {

namespace {

// Non-throwing parse of a WAM string parameter id (e.g. "3") to a ParamID.
// WAM ids arrive from untrusted host/JS input and are decoded on the audio
// render thread, so this must never throw: under -fno-exceptions a thrown
// std::stoi would abort the AudioWorklet. Returns nullopt on malformed input.
std::optional<state::ParamID> parse_param_id(const std::string& id) {
    if (id.empty()) return std::nullopt;
    unsigned long long value = 0;
    const char* begin = id.data();
    const char* end = begin + id.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return static_cast<state::ParamID>(value);
}

// Append a JSON-escaped copy of `s` to `ss`. A PluginDescriptor name/vendor can
// contain a quote or backslash, which would otherwise produce invalid JSON.
void append_json_escaped(std::ostringstream& ss, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':  ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    ss << buf;
                } else {
                    ss << c;
                }
        }
    }
}

} // namespace

// ── WamDescriptorData ───────────────────────────────────────────────────

WamDescriptorData WamDescriptorData::from_processor(const PluginDescriptor& desc) {
    WamDescriptorData d;
    d.name = desc.name;
    d.vendor = desc.manufacturer;
    d.version = desc.version;
    d.is_instrument = (desc.category == PluginCategory::Instrument);
    d.has_audio_input = desc.default_input_channels() > 0;
    d.has_audio_output = desc.default_output_channels() > 0;
    d.has_midi_input = desc.accepts_midi;
    d.has_midi_output = desc.produces_midi;
    d.has_automation_input = true;
    return d;
}

std::string WamDescriptorData::to_json() const {
    std::ostringstream ss;
    ss << "{";
    ss << "\"name\":\""; append_json_escaped(ss, name); ss << "\",";
    ss << "\"vendor\":\""; append_json_escaped(ss, vendor); ss << "\",";
    ss << "\"version\":\""; append_json_escaped(ss, version); ss << "\",";
    ss << "\"apiVersion\":\""; append_json_escaped(ss, api_version); ss << "\",";
    ss << "\"isInstrument\":" << (is_instrument ? "true" : "false") << ",";
    ss << "\"hasAudioInput\":" << (has_audio_input ? "true" : "false") << ",";
    ss << "\"hasAudioOutput\":" << (has_audio_output ? "true" : "false") << ",";
    ss << "\"hasMidiInput\":" << (has_midi_input ? "true" : "false") << ",";
    ss << "\"hasMidiOutput\":" << (has_midi_output ? "true" : "false") << ",";
    ss << "\"hasAutomationInput\":" << (has_automation_input ? "true" : "false") << ",";
    ss << "\"hasAutomationOutput\":" << (has_automation_output ? "true" : "false") << ",";
    ss << "\"hasMpeInput\":" << (has_mpe_input ? "true" : "false") << ",";
    ss << "\"hasMpeOutput\":" << (has_mpe_output ? "true" : "false");
    ss << "}";
    return ss.str();
}

// ── WamProcessorBridge ──────────────────────────────────────────────────

WamProcessorBridge::WamProcessorBridge(ProcessorFactory factory)
    : factory_(factory) {}

WamProcessorBridge::~WamProcessorBridge() = default;

bool WamProcessorBridge::initialize(double sample_rate, int max_block_size) {
    processor_ = factory_();
    if (!processor_) return false;

    processor_->set_state_store(&store_);
    processor_->define_parameters(store_);

    auto desc = processor_->descriptor();
    sample_rate_ = sample_rate;
    num_channels_ = (std::max)(desc.default_input_channels(),
                               desc.default_output_channels());
    if (num_channels_ < 1) num_channels_ = 2;
    block_size_ = max_block_size;

    PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_buffer_size = max_block_size;
    ctx.input_channels = desc.default_input_channels();
    ctx.output_channels = desc.default_output_channels();
    processor_->prepare(ctx);

    // Pre-allocate planar buffers
    input_planar_.resize(num_channels_ * max_block_size, 0.0f);
    output_planar_.resize(num_channels_ * max_block_size, 0.0f);
    input_ptrs_.resize(num_channels_);
    output_ptrs_.resize(num_channels_);

    for (int ch = 0; ch < num_channels_; ++ch) {
        input_ptrs_[ch] = input_planar_.data() + ch * max_block_size;
        output_ptrs_[ch] = output_planar_.data() + ch * max_block_size;
    }

    runtime::log_info("WAMv2: initialized '{}' at {} Hz, {} channels",
                      desc.name, sample_rate, num_channels_);
    return true;
}

void WamProcessorBridge::process(const float* input, float* output,
                                  int num_channels, int num_frames) {
    if (!processor_) return;

    int ch = (std::min)(num_channels, num_channels_);

    // Bound the frame count to the planar buffers allocated in initialize().
    // Web Audio's render quantum is fixed at 128 today, but the host passes
    // num_frames explicitly: a larger value would overrun input_ptrs_/
    // output_ptrs_ (each sized to block_size_). Clamp rather than corrupt.
    if (num_frames > block_size_) num_frames = block_size_;
    if (num_frames <= 0) return;

    // De-interleave: Web Audio [L0,R0,L1,R1,...] → planar [L0,L1,...][R0,R1,...]
    for (int f = 0; f < num_frames; ++f) {
        for (int c = 0; c < ch; ++c) {
            input_ptrs_[c][f] = input[f * num_channels + c];
        }
    }

    audio::BufferView<const float> in_view(
        const_cast<const float* const*>(input_ptrs_.data()), ch, num_frames);
    audio::BufferView<float> out_view(output_ptrs_.data(), ch, num_frames);

    // Process MIDI
    midi::MidiBuffer midi_in = std::move(pending_midi_);
    midi::MidiBuffer midi_out;
    pending_midi_.clear();

    ProcessContext ctx;
    ctx.sample_rate = sample_rate_;
    ctx.num_samples = num_frames;
    ctx.process_mode = ProcessMode::Realtime;
    ctx.render_speed_hint = RenderSpeedHint::Realtime;

    processor_->process(out_view, in_view, midi_in, midi_out, ctx);

    // Re-interleave: planar → Web Audio interleaved
    for (int f = 0; f < num_frames; ++f) {
        for (int c = 0; c < ch; ++c) {
            output[f * num_channels + c] = output_ptrs_[c][f];
        }
    }
}

std::vector<WamParamInfo> WamProcessorBridge::get_parameter_info() const {
    std::vector<WamParamInfo> result;
    for (const auto& p : store_.all_params()) {
        WamParamInfo info;
        info.id = std::to_string(p.id);
        info.label = p.name;
        info.unit = p.unit;
        info.default_value = p.range.default_value;
        info.min_value = p.range.min;
        info.max_value = p.range.max;
        info.step = p.range.step;
        info.discrete_step = (p.range.step >= 1.0f) ? static_cast<int>(p.range.step) : 0;

        // WAMv2 type classification
        if (p.range.step >= 1.0f && p.range.min == 0.0f && p.range.max == 1.0f)
            info.type = "boolean";
        else if (p.range.step >= 1.0f)
            info.type = "int";
        else
            info.type = "float";

        result.push_back(std::move(info));
    }
    return result;
}

std::string WamProcessorBridge::parameters_json() const {
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (const auto& p : get_parameter_info()) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"id\":\""; append_json_escaped(ss, p.id);
        ss << "\",\"label\":\""; append_json_escaped(ss, p.label);
        ss << "\",\"type\":\""; append_json_escaped(ss, p.type);
        ss << "\",\"unit\":\""; append_json_escaped(ss, p.unit);
        ss << "\",\"defaultValue\":" << p.default_value
           << ",\"minValue\":" << p.min_value
           << ",\"maxValue\":" << p.max_value
           << ",\"step\":" << p.step
           << ",\"discreteStep\":" << p.discrete_step << "}";
    }
    ss << "]";
    return ss.str();
}

float WamProcessorBridge::get_parameter_value(const std::string& id) const {
    auto param_id = parse_param_id(id);
    if (!param_id) return 0.0f;
    return store_.get_value(*param_id);
}

void WamProcessorBridge::set_parameter_value(const std::string& id, float value) {
    auto param_id = parse_param_id(id);
    if (!param_id) return;
    store_.set_value(*param_id, value);
}

void WamProcessorBridge::schedule_midi(uint8_t status, uint8_t data1,
                                        uint8_t data2, int sample_offset) {
    midi::MidiEvent event;
    if ((status & 0xF0) == 0x90 && data2 > 0)
        event = midi::MidiEvent::note_on(status & 0x0F, data1, data2);
    else if ((status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && data2 == 0))
        event = midi::MidiEvent::note_off(status & 0x0F, data1, data2);
    else if ((status & 0xF0) == 0xB0)
        event = midi::MidiEvent::cc(status & 0x0F, data1, data2);
    else
        return;

    event.sample_offset = sample_offset;
    pending_midi_.add(event);
}

std::vector<uint8_t> WamProcessorBridge::get_state() const {
    return store_.serialize();
}

bool WamProcessorBridge::set_state(const uint8_t* data, size_t size) {
    return store_.deserialize({data, size});
}

WamDescriptorData WamProcessorBridge::descriptor() const {
    if (!processor_) return {};
    return WamDescriptorData::from_processor(processor_->descriptor());
}

} // namespace pulp::format::wam

// Emscripten exports are provided by per-plugin entry point files
// (e.g., pulp_gain_wasm.cpp) which create a WamProcessorBridge
// and expose the wam_init/wam_process/wam_set_param/etc. C exports.
