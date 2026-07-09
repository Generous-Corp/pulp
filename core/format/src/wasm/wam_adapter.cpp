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
#include <array>
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
    // tail_samples lives on the descriptor; latency_samples is a Processor
    // method, so WamProcessorBridge::descriptor() fills d.latency_samples in.
    d.tail_samples = desc.tail_samples;
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
    ss << "\"hasMpeOutput\":" << (has_mpe_output ? "true" : "false") << ",";
    ss << "\"latencySamples\":" << latency_samples << ",";
    ss << "\"tailSamples\":" << tail_samples;
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

    // Reserve the MIDI buffers once, then forbid them from growing. Previously
    // process() default-constructed a MidiBuffer every block and let midi_in /
    // midi_out reallocate under add() — both allocations on the audio thread.
    pending_midi_.reserve(kMidiEventCapacity, kSysexCapacity, kSysexPayloadCapacity);
    pending_midi_.set_realtime_capacity_limit(true);
    midi_out_.reserve(kMidiEventCapacity, kSysexCapacity, kSysexPayloadCapacity);
    midi_out_.set_realtime_capacity_limit(true);

    // Worst case per record: 4-byte offset + 2-byte length + payload.
    constexpr std::size_t kRecordHeader = sizeof(int32_t) + sizeof(uint16_t);
    midi_out_scratch_.assign(
        kMidiEventCapacity * (kRecordHeader + 3)
            + kSysexCapacity * (kRecordHeader + kSysexPayloadCapacity),
        0);
    midi_out_bytes_ = 0;

    runtime::log_info("WAMv2: initialized '{}' at {} Hz, {} channels",
                      desc.name, sample_rate, num_channels_);
    return true;
}

namespace {

// Append one [int32 offset][uint16 len][bytes] record. Returns false (and writes
// nothing) when the scratch is full — bounded, allocation-free, audio-thread safe.
bool append_midi_record(std::vector<uint8_t>& scratch, std::size_t& used,
                        int32_t sample_offset, const uint8_t* bytes,
                        std::size_t len) {
    if (len == 0 || len > 0xFFFF) return false;
    const std::size_t need = sizeof(int32_t) + sizeof(uint16_t) + len;
    if (used + need > scratch.size()) return false;

    uint8_t* dst = scratch.data() + used;
    const auto off = static_cast<int32_t>(sample_offset);
    const auto len16 = static_cast<uint16_t>(len);
    std::memcpy(dst, &off, sizeof(off));
    std::memcpy(dst + sizeof(off), &len16, sizeof(len16));
    std::memcpy(dst + sizeof(off) + sizeof(len16), bytes, len);
    used += need;
    return true;
}

} // namespace

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

    // MIDI. pending_midi_ IS midi_in — no per-block MidiBuffer construction and
    // no move that strips its reserved capacity.
    midi_out_.clear();
    midi_out_.clear_sysex();
    midi_out_bytes_ = 0;

    ProcessContext ctx;
    ctx.sample_rate = sample_rate_;
    ctx.num_samples = num_frames;
    ctx.process_mode = ProcessMode::Realtime;
    ctx.render_speed_hint = RenderSpeedHint::Realtime;

    // Host-supplied transport (Web Audio has none of its own). Fields the web
    // host cannot fill keep ProcessContext's defaults.
    ctx.is_playing = transport_.is_playing;
    ctx.tempo_bpm = transport_.tempo_bpm;
    ctx.position_beats = transport_.position_beats;
    ctx.position_samples = transport_.position_samples;
    ctx.time_sig_numerator = transport_.time_sig_numerator;
    ctx.time_sig_denominator = transport_.time_sig_denominator;

    // One-shot reset: Processor has no reset() virtual, so request a DSP-state
    // reset via the ProcessContext contract for exactly this block, then clear
    // the flag so it is not sticky. RT-safe — just a bool read + clear.
    if (reset_pending_) {
        ctx.reset_requested = true;
        reset_pending_ = false;
    }

    processor_->process(out_view, in_view, pending_midi_, midi_out_, ctx);

    // Serialize whatever the processor emitted, so drain_midi_out() is a plain
    // memcpy. Previously midi_out simply went out of scope here and every MIDI
    // event a plugin produced was discarded — while the descriptor advertised
    // hasMidiOutput: true.
    for (const auto& event : midi_out_) {
        append_midi_record(midi_out_scratch_, midi_out_bytes_,
                           event.sample_offset, event.data(), event.size());
    }
    for (const auto& event : midi_out_.sysex()) {
        append_midi_record(midi_out_scratch_, midi_out_bytes_,
                           event.sample_offset, event.data.data(),
                           event.data.size());
    }

    // The block consumed its input events.
    pending_midi_.clear();
    pending_midi_.clear_sysex();

    // Re-interleave: planar → Web Audio interleaved
    for (int f = 0; f < num_frames; ++f) {
        for (int c = 0; c < ch; ++c) {
            output[f * num_channels + c] = output_ptrs_[c][f];
        }
    }
}

int WamProcessorBridge::drain_midi_out(uint8_t* dst, int cap) const {
    const auto available = static_cast<int>(midi_out_bytes_);
    if (dst && cap > 0 && available > 0) {
        std::memcpy(dst, midi_out_scratch_.data(),
                    static_cast<std::size_t>((std::min)(cap, available)));
    }
    return available;   // > cap tells the caller the tail was truncated
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

        // A stepped parameter that declares a display formatter is a choice:
        // ask it to name each discrete value. Bounded so a pathological range
        // (e.g. 0..100000 step 1) cannot generate a colossal JSON payload.
        constexpr int kMaxEnumLabels = 64;
        if (p.range.step >= 1.0f && p.to_string) {
            const int count = static_cast<int>(
                (p.range.max - p.range.min) / p.range.step) + 1;
            if (count >= 2 && count <= kMaxEnumLabels) {
                info.value_labels.reserve(static_cast<std::size_t>(count));
                for (int i = 0; i < count; ++i) {
                    info.value_labels.push_back(
                        p.to_string(p.range.min + static_cast<float>(i) * p.range.step));
                }
            }
        }

        // WAMv2 type classification
        if (p.range.step >= 1.0f && p.range.min == 0.0f && p.range.max == 1.0f)
            info.type = "boolean";
        else if (!info.value_labels.empty())
            info.type = "choice";
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
           << ",\"discreteStep\":" << p.discrete_step;
        if (!p.value_labels.empty()) {
            ss << ",\"labels\":[";
            bool first_label = true;
            for (const auto& label : p.value_labels) {
                if (!first_label) ss << ",";
                first_label = false;
                ss << "\""; append_json_escaped(ss, label); ss << "\"";
            }
            ss << "]";
        }
        ss << "}";
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

    // Note-on with velocity 0 is a note-off; normalize it so downstream
    // is_note_off() checks behave the same as they do on a hardware port.
    if ((status & 0xF0) == 0x90 && data2 == 0) {
        event = midi::MidiEvent::note_off(status & 0x0F, data1, 0);
    } else {
        // Everything else passes through verbatim. This previously `return`ed
        // for any status outside note-on/note-off/CC, silently swallowing
        // pitch-bend (0xE0), program change (0xC0), channel pressure (0xD0)
        // and poly aftertouch (0xA0) — so, e.g., synth-with-presets' pitch
        // bend never reached the DSP in a browser.
        event = midi::MidiEvent{choc::midi::ShortMessage(status, data1, data2),
                                0, 0.0};
    }

    event.sample_offset = sample_offset;
    pending_midi_.add(event);   // drops (counted) rather than growing when full
}

bool WamProcessorBridge::schedule_sysex(const uint8_t* data, int size,
                                         int sample_offset) {
    if (!data || size <= 0) return false;
    // add_sysex_copy() honours set_realtime_capacity_limit(): it takes a payload
    // from the reserved pool and drops when none is large enough or free.
    return pending_midi_.add_sysex_copy(data, static_cast<std::size_t>(size),
                                        sample_offset);
}

// Composed web state container. Previously get_state()/set_state() serialized
// only the StateStore, so Processor::serialize_plugin_state() was never called
// and any non-parameter state (state-memo's free-text memo, for instance) was
// silently lost across a save/load in the browser.
//
//   "PWS1"                                    magic + container version
//   [u32 params_len][params_len bytes]        store_.serialize()  (starts "PULP")
//   [u32 plugin_len][plugin_len bytes]        serialize_plugin_state()
//
// Blobs that do not begin with the magic are treated as legacy params-only
// state (which is exactly what older builds wrote) and still load.
namespace {

constexpr std::array<uint8_t, 4> kWebStateMagic{'P', 'W', 'S', '1'};

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    const auto le = value;   // wasm is little-endian; keep the wire LE explicitly
    out.push_back(static_cast<uint8_t>(le & 0xFF));
    out.push_back(static_cast<uint8_t>((le >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((le >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((le >> 24) & 0xFF));
}

bool read_u32(const uint8_t* data, size_t size, size_t& pos, uint32_t& out) {
    if (pos + 4 > size) return false;
    out = static_cast<uint32_t>(data[pos])
        | (static_cast<uint32_t>(data[pos + 1]) << 8)
        | (static_cast<uint32_t>(data[pos + 2]) << 16)
        | (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

bool has_web_state_magic(const uint8_t* data, size_t size) {
    return size >= kWebStateMagic.size()
        && std::equal(kWebStateMagic.begin(), kWebStateMagic.end(), data);
}

} // namespace

std::vector<uint8_t> WamProcessorBridge::get_state() const {
    const auto params = store_.serialize();
    const auto plugin = processor_ ? processor_->serialize_plugin_state()
                                   : std::vector<uint8_t>{};

    std::vector<uint8_t> out;
    out.reserve(kWebStateMagic.size() + 8 + params.size() + plugin.size());
    out.insert(out.end(), kWebStateMagic.begin(), kWebStateMagic.end());
    append_u32(out, static_cast<uint32_t>(params.size()));
    out.insert(out.end(), params.begin(), params.end());
    append_u32(out, static_cast<uint32_t>(plugin.size()));
    out.insert(out.end(), plugin.begin(), plugin.end());
    return out;
}

bool WamProcessorBridge::set_state(const uint8_t* data, size_t size) {
    if (!data || size == 0) return false;

    // Legacy: a bare StateStore blob written before the container existed.
    if (!has_web_state_magic(data, size)) {
        const bool ok = store_.deserialize({data, size});
        // Tell the plugin its owned state carried no payload, per the
        // Processor contract ("empty span" => reset to defaults).
        if (processor_) processor_->deserialize_plugin_state({});
        return ok;
    }

    size_t pos = kWebStateMagic.size();
    uint32_t params_len = 0;
    if (!read_u32(data, size, pos, params_len)) return false;
    if (pos + params_len > size) return false;
    const bool params_ok = store_.deserialize({data + pos, params_len});
    pos += params_len;

    uint32_t plugin_len = 0;
    if (!read_u32(data, size, pos, plugin_len)) return false;
    if (pos + plugin_len > size) return false;

    bool plugin_ok = true;
    if (processor_) {
        plugin_ok = processor_->deserialize_plugin_state(
            {data + pos, static_cast<std::size_t>(plugin_len)});
    }
    return params_ok && plugin_ok;
}

WamDescriptorData WamProcessorBridge::descriptor() const {
    if (!processor_) return {};
    auto d = WamDescriptorData::from_processor(processor_->descriptor());
    // latency_samples() is a Processor method, not a descriptor field, so it is
    // filled here rather than in from_processor().
    d.latency_samples = processor_->latency_samples();
    return d;
}

int WamProcessorBridge::latency_samples() const {
    return processor_ ? processor_->latency_samples() : 0;
}

void WamProcessorBridge::prepare(double sample_rate, int block_size) {
    // CONTROL THREAD ONLY. Re-runs Processor::prepare() and may resize the
    // planar buffers — both allocate. Never call from process(); in the worklet
    // this is a port message serviced between render quanta.
    if (!processor_ || block_size <= 0) return;

    sample_rate_ = sample_rate;
    auto desc = processor_->descriptor();

    PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_buffer_size = block_size;
    ctx.input_channels = desc.default_input_channels();
    ctx.output_channels = desc.default_output_channels();
    processor_->prepare(ctx);

    // Grow the planar buffers only when the block size actually increased, so a
    // same-or-smaller block never reallocates. The de-interleave stride is
    // block_size_, so it and the channel pointers move together on a grow.
    if (block_size > block_size_) {
        block_size_ = block_size;
        input_planar_.assign(static_cast<std::size_t>(num_channels_) * block_size_, 0.0f);
        output_planar_.assign(static_cast<std::size_t>(num_channels_) * block_size_, 0.0f);
        for (int ch = 0; ch < num_channels_; ++ch) {
            input_ptrs_[ch] = input_planar_.data() + ch * block_size_;
            output_ptrs_[ch] = output_planar_.data() + ch * block_size_;
        }
    }
}

void WamProcessorBridge::set_transport(bool is_playing, double bpm,
                                        double position_beats,
                                        double position_samples, int tsig_num,
                                        int tsig_den) noexcept {
    transport_.is_playing = is_playing;
    transport_.tempo_bpm = bpm;
    transport_.position_beats = position_beats;
    transport_.position_samples = static_cast<int64_t>(position_samples);
    transport_.time_sig_numerator = tsig_num;
    transport_.time_sig_denominator = tsig_den;
}

} // namespace pulp::format::wam

// Emscripten exports are provided by per-plugin entry point files
// (e.g., pulp_gain_wasm.cpp) which create a WamProcessorBridge
// and expose the wam_init/wam_process/wam_set_param/etc. C exports.
