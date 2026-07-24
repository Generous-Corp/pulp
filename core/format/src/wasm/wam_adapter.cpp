// WAMv2 format adapter implementation
// Bridges Pulp Processor to WAMv2 AudioWorkletProcessor via Emscripten.
//
// Audio thread flow (PLANAR end-to-end — the ABI passes planar channel-pointer
// arrays, the same shape WCLAP and native CLAP use, so there is no interleave
// round-trip):
//   JS AudioWorkletProcessor.process()
//     → C++ WamProcessorBridge::process(inputs[], outputs[], ch, frames)
//       → Pulp Processor::process()  (renders inputs[] → outputs[] directly)

#include <pulp/format/web/wam_adapter.hpp>
#include <pulp/format/plugin_state_io.hpp>
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
#include <utility>

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

// Build the WAMv2 parameter metadata for one StateStore ParamInfo. Shared by
// the single-plugin bridge and every stage of a rack so the enum-label /
// type-classification logic lives in exactly one place. The `id` field is left
// empty here — the caller stamps it (a bare "6" for a single plugin, a
// stage-qualified "1:6" for a rack).
WamParamInfo make_param_info(const state::ParamInfo& p) {
    WamParamInfo info;
    info.label = p.name;
    info.unit = p.unit;
    info.default_value = p.range.default_value;
    info.min_value = p.range.min;
    info.max_value = p.range.max;
    info.step = p.range.step;
    info.discrete_step = (p.range.step >= 1.0f) ? static_cast<int>(p.range.step) : 0;

    // A stepped parameter that declares a display formatter is a choice: ask it
    // to name each discrete value. Bounded so a pathological range cannot
    // generate a colossal JSON payload.
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

    if (p.range.step >= 1.0f && p.range.min == 0.0f && p.range.max == 1.0f)
        info.type = "boolean";
    else if (!info.value_labels.empty())
        info.type = "choice";
    else if (p.range.step >= 1.0f)
        info.type = "int";
    else
        info.type = "float";

    return info;
}

// Append one parameter's JSON object to `ss` using an already-stamped id. When
// `stage >= 0` a "stage" field is emitted so a rack host can group controls.
void append_param_json(std::ostringstream& ss, const WamParamInfo& p, int stage) {
    ss << "{\"id\":\""; append_json_escaped(ss, p.id);
    ss << "\",\"label\":\""; append_json_escaped(ss, p.label);
    ss << "\",\"type\":\""; append_json_escaped(ss, p.type);
    ss << "\",\"unit\":\""; append_json_escaped(ss, p.unit);
    ss << "\",\"defaultValue\":" << p.default_value
       << ",\"minValue\":" << p.min_value
       << ",\"maxValue\":" << p.max_value
       << ",\"step\":" << p.step
       << ",\"discreteStep\":" << p.discrete_step;
    if (stage >= 0) ss << ",\"stage\":" << stage;
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
    // A rack fills `stages` with its per-stage names; a single plugin leaves it
    // empty, so this whole clause vanishes and the object is byte-for-byte the
    // single-plugin descriptor it always was.
    if (!stages.empty()) {
        ss << ",\"stages\":[";
        bool first_stage = true;
        for (const auto& stage_name : stages) {
            if (!first_stage) ss << ",";
            first_stage = false;
            ss << "\""; append_json_escaped(ss, stage_name); ss << "\"";
        }
        ss << "]";
    }
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

    // Count every parameter change, including the plugin's own writes from
    // inside process() (synth-with-presets loads a factory preset into its
    // timbre parameters when Program changes). An Audio listener fires inline,
    // allocation-free, on the thread that performed the write.
    param_listener_ = store_.add_audio_listener(
        [this](state::ParamID, float) {
            param_epoch_.fetch_add(1, std::memory_order_relaxed);
        });

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

    // No planar scratch to allocate: process() renders directly from the host's
    // planar input channel pointers into the host's planar output pointers.

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

// ── Shared bridge helpers ────────────────────────────────────────────────
// The single-plugin bridge and the rack ran byte-identical copies of these; the
// evidence in the audit (twin set_transport / schedule_midi / drain_midi_out /
// read_param_values bodies) is exactly what these collapse. Free functions over
// the members each bridge already owns, so there is one implementation to test.

// Copy a host transport snapshot into the bridge's WamTransport POD.
void store_transport(WamTransport& t, bool is_playing, double bpm,
                     double position_beats, double position_samples,
                     int tsig_num, int tsig_den) noexcept {
    t.valid = true;
    t.is_playing = is_playing;
    t.tempo_bpm = bpm;
    t.position_beats = position_beats;
    t.position_samples = static_cast<int64_t>(position_samples);
    t.time_sig_numerator = tsig_num;
    t.time_sig_denominator = tsig_den;
}

// Decode one 3-byte WAM MIDI message into `pending` (feeds a bridge's midi_in).
// Note-on velocity 0 is normalized to note-off; everything else passes through
// verbatim (pitch-bend / program-change / pressure must reach the DSP). Drops
// (counted) rather than growing when the RT-capacity-limited buffer is full.
void schedule_short_midi(midi::MidiBuffer& pending, uint8_t status, uint8_t data1,
                         uint8_t data2, int sample_offset) {
    midi::MidiEvent event;
    if ((status & 0xF0) == 0x90 && data2 == 0) {
        event = midi::MidiEvent::note_off(status & 0x0F, data1, 0);
    } else {
        event = midi::MidiEvent{choc::midi::ShortMessage(status, data1, data2),
                                0, 0.0};
    }
    event.sample_offset = sample_offset;
    pending.add(event);
}

// Copy a variable-length SysEx payload into `pending` from the reserved pool.
bool schedule_sysex_into(midi::MidiBuffer& pending, const uint8_t* data, int size,
                         int sample_offset) {
    if (!data || size <= 0) return false;
    return pending.add_sysex_copy(data, static_cast<std::size_t>(size),
                                  sample_offset);
}

// Serialize every event a processor emitted (short messages first, then SysEx)
// into `scratch` as packed [offset][len][bytes] records, so drain_midi_out() is
// a plain memcpy. Resets `used` to 0 first — one block's worth of output.
void serialize_midi_out(std::vector<uint8_t>& scratch, std::size_t& used,
                        const midi::MidiBuffer& out) {
    used = 0;
    for (const auto& event : out)
        append_midi_record(scratch, used, event.sample_offset,
                           event.data(), event.size());
    for (const auto& event : out.sysex())
        append_midi_record(scratch, used, event.sample_offset,
                           event.data.data(), event.data.size());
}

// Copy min(cap, available) serialized MIDI-out bytes into `dst`; returns the
// count AVAILABLE (a return > cap tells the caller the tail was truncated).
int drain_records(const std::vector<uint8_t>& scratch, std::size_t used,
                  uint8_t* dst, int cap) {
    const auto available = static_cast<int>(used);
    if (dst && cap > 0 && available > 0) {
        std::memcpy(dst, scratch.data(),
                    static_cast<std::size_t>((std::min)(cap, available)));
    }
    return available;
}

// Bulk-read a StateStore's values in all_params() declaration order. Writes
// min(count, capacity) floats and returns the total parameter count.
int read_store_values(const state::StateStore& store, float* dst, int capacity) {
    const auto params = store.all_params();
    const int count = static_cast<int>(params.size());
    if (!dst || capacity <= 0) return count;
    const int n = (std::min)(count, capacity);
    for (int i = 0; i < n; ++i)
        dst[i] = store.get_value(params[static_cast<std::size_t>(i)].id);
    return count;
}

} // namespace

void WamProcessorBridge::process(const float* const* inputs,
                                  float* const* outputs,
                                  int num_channels, int num_frames) {
    if (!processor_) return;

    const int ch = (std::min)(num_channels, num_channels_);

    // Bound the frame count. Web Audio's render quantum is fixed at 128 today,
    // but the host passes num_frames explicitly and the JS side allocates its
    // channel buffers sized to block_size_; a larger value would read/write past
    // them. Clamp rather than corrupt.
    if (num_frames > block_size_) num_frames = block_size_;
    if (num_frames <= 0) return;

    // Planar in, planar out — no interleave. `inputs`/`outputs` are the host's
    // per-channel pointer arrays (same layout WCLAP/native CLAP pass), so the
    // processor renders directly from and into them.
    audio::BufferView<const float> in_view(
        inputs, static_cast<std::size_t>(ch),
        static_cast<std::size_t>(num_frames));
    audio::BufferView<float> out_view(
        outputs, static_cast<std::size_t>(ch),
        static_cast<std::size_t>(num_frames));

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
    if (transport_.valid) {
        ctx.transport_validity.set(TransportField::Playing);
        ctx.transport_validity.set(TransportField::Tempo);
        ctx.transport_validity.set(TransportField::BeatPosition);
        ctx.transport_validity.set(TransportField::SamplePosition);
        ctx.transport_validity.set(TransportField::TimeSignature);
    }

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
    serialize_midi_out(midi_out_scratch_, midi_out_bytes_, midi_out_);

    // The block consumed its input events.
    pending_midi_.clear();
    pending_midi_.clear_sysex();

    // Output is already in the host's planar buffers — nothing to interleave.

    // COALESCED non-realtime pass, AFTER this block's audio is produced. Every
    // control message that arrived since the last block collapses into at most
    // ONE reconcile of the latest values (see service_non_realtime /
    // WamStage::service_non_realtime). It runs outside Processor::process() but
    // on the same OS thread — the worklet has no other — so a processor's tick
    // still has to keep its work bounded, it just is not asked to do it N times.
    service_non_realtime();
}

int WamStage::read_param_values(float* dst, int capacity) const {
    return read_store_values(store_, dst, capacity);
}

uint32_t WamProcessorBridge::param_epoch() const {
    return param_epoch_.load(std::memory_order_relaxed);
}

int WamProcessorBridge::read_param_values(float* dst, int capacity) const {
    return read_store_values(store_, dst, capacity);
}

int WamProcessorBridge::drain_midi_out(uint8_t* dst, int cap) const {
    return drain_records(midi_out_scratch_, midi_out_bytes_, dst, cap);
}

std::vector<WamParamInfo> WamProcessorBridge::get_parameter_info() const {
    std::vector<WamParamInfo> result;
    for (const auto& p : store_.all_params()) {
        WamParamInfo info = make_param_info(p);
        info.id = std::to_string(p.id);
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
        append_param_json(ss, p, /*stage=*/-1);
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

    // Only MARK the derived state dirty. `wam_set_param` reaches us from the
    // worklet's port.onmessage — which is dispatched on the RENDER THREAD
    // between quanta — and a knob drag delivers a burst of writes in one turn.
    // A processor that derives heavy state from a parameter (SuperConvolver
    // rebuilds its impulse response when `Size` moves) would then do the whole
    // rebuild once per message, with only the last result ever audible. The
    // rebuild is coalesced into ONE pass per block in service_non_realtime().
    tick_pending_ = true;
}

void WamProcessorBridge::service_non_realtime() {
    if (!processor_) return;
    if (!tick_pending_ && !processor_->non_realtime_tick_pending()) return;
    tick_pending_ = false;
    processor_->on_non_realtime_tick();
}

void WamProcessorBridge::schedule_midi(uint8_t status, uint8_t data1,
                                        uint8_t data2, int sample_offset) {
    schedule_short_midi(pending_midi_, status, data1, data2, sample_offset);
}

bool WamProcessorBridge::schedule_sysex(const uint8_t* data, int size,
                                         int sample_offset) {
    return schedule_sysex_into(pending_midi_, data, size, sample_offset);
}

// Single-plugin state is the framework's shared plugin_state_io format (see
// encode_stage_state / decode_stage_state below): a versioned, CRC-checked
// "PLST" envelope composing StateStore params + Processor::serialize_plugin_state()
// (so state-memo's free-text memo survives a browser save/load), or a bare
// "PULP" StateStore blob when the plugin owns no extra state. It is byte-for-byte
// the format the native VST3/AU/CLAP builds read and write. Only the multi-stage
// RACK framing below ("PWR1") is web-specific; each stage payload is a plugin_state_io blob.
namespace {

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    const auto le = value;   // wasm is little-endian; keep the wire LE explicitly
    out.push_back(static_cast<uint8_t>(le & 0xFF));
    out.push_back(static_cast<uint8_t>((le >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((le >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((le >> 24) & 0xFF));
}

bool read_u32(const uint8_t* data, size_t size, size_t& pos, uint32_t& out) {
    // Wrap-safe: `pos + 4` would overflow on wasm32 for a hostile `pos`. The
    // subtractive form can't wrap because `pos <= size` is the invariant every
    // caller maintains (each read advances `pos` only after this guard passes).
    if (pos > size || size - pos < 4) return false;
    out = static_cast<uint32_t>(data[pos])
        | (static_cast<uint32_t>(data[pos + 1]) << 8)
        | (static_cast<uint32_t>(data[pos + 2]) << 16)
        | (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

// The magic for the rack container: "PWR1" [u32 stage_count] then, per stage,
// [u32 len][a per-stage plugin_state_io blob]. A stage blob is byte-identical
// to a standalone plugin's save, so one stage of a rack and a single-plugin
// save are interchangeable at the container level. Only the outer multi-stage
// framing is web-specific; each stage payload is the shared "PLST" envelope.
constexpr std::array<uint8_t, 4> kRackStateMagic{'P', 'W', 'R', '1'};

bool has_rack_state_magic(const uint8_t* data, size_t size) {
    return size >= kRackStateMagic.size()
        && std::equal(kRackStateMagic.begin(), kRackStateMagic.end(), data);
}

// Serialize one plugin's host-facing state (StateStore params + Processor
// plugin-owned blob). Delegates to the framework's shared `plugin_state_io`
// envelope ("PLST" + CRC + versioning) — the SAME format VST3/AU/CLAP/headless
// write — so a state blob saved in the browser round-trips through a native
// build of the same plugin and vice versa. (`plugin_state_io::serialize`
// returns a bare StateStore blob when the plugin-owned payload is empty, which
// is the common case for parameter-only effects.) Single source of truth for
// both the single-plugin bridge and each stage of a rack.
std::vector<uint8_t> encode_stage_state(const state::StateStore& store,
                                        const Processor* processor) {
    if (!processor) return store.serialize();
    return plugin_state_io::serialize(store, *processor);
}

// Restore one plugin from a `plugin_state_io` blob. `deserialize` accepts both
// the "PLST" envelope and a legacy bare StateStore blob, verifies the CRC, and
// rolls the live StateStore back to its prior contents if the restore fails —
// so a truncated or corrupt blob can never half-apply. It is wrap-safe on
// wasm32 (exact-size envelope check, subtractive bounds arithmetic).
bool decode_stage_state(state::StateStore& store, Processor* processor,
                        const uint8_t* data, size_t size) {
    if (!data || size == 0 || !processor) return false;
    return plugin_state_io::deserialize({data, size}, store, *processor);
}

} // namespace

std::vector<uint8_t> WamProcessorBridge::get_state() const {
    return encode_stage_state(store_, processor_.get());
}

bool WamProcessorBridge::set_state(const uint8_t* data, size_t size) {
    const bool ok = decode_stage_state(store_, processor_.get(), data, size);
    // A restored state can name a different IR / wavetable / sample source than
    // the live one. Mark it dirty; the next block's service_non_realtime()
    // reconciles it (one pass, coalesced with whatever parameter writes a host
    // sends alongside a project load) — without that, the audio thread would
    // keep rendering the OLD derived state for the rest of the session.
    tick_pending_ = true;
    return ok;
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
    // Runs BETWEEN render quanta, not on a separate control thread: in an
    // AudioWorkletProcessor the port message that reaches here is dispatched on
    // the audio render thread itself. Re-runs Processor::prepare(), which may
    // allocate, so never call from process().
    if (!processor_ || block_size <= 0) return;

    sample_rate_ = sample_rate;
    auto desc = processor_->descriptor();

    PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_buffer_size = block_size;
    ctx.input_channels = desc.default_input_channels();
    ctx.output_channels = desc.default_output_channels();
    processor_->prepare(ctx);

    // Track the largest block the processor is prepared for; process() clamps
    // num_frames to it. No planar scratch to resize — the JS side owns the
    // channel buffers now.
    if (block_size > block_size_) block_size_ = block_size;

    // prepare() may have invalidated derived state (a new sample rate re-decodes
    // and re-resamples an IR). Same non-render context as above.
    processor_->on_non_realtime_tick();
}

void WamProcessorBridge::set_transport(bool is_playing, double bpm,
                                        double position_beats,
                                        double position_samples, int tsig_num,
                                        int tsig_den) noexcept {
    store_transport(transport_, is_playing, bpm, position_beats,
                    position_samples, tsig_num, tsig_den);
}

// ── WamStage ────────────────────────────────────────────────────────────

bool WamStage::initialize(std::unique_ptr<Processor> processor,
                          double sample_rate, int max_block_size) {
    processor_ = std::move(processor);
    if (!processor_) return false;

    processor_->set_state_store(&store_);
    processor_->define_parameters(store_);

    // See WamProcessorBridge::initialize — a stage's plugin can rewrite its own
    // parameters from inside process(), and the host must be able to notice.
    param_listener_ = store_.add_audio_listener(
        [this](state::ParamID, float) {
            param_epoch_.fetch_add(1, std::memory_order_relaxed);
        });

    auto desc = processor_->descriptor();
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

    output_planar_.assign(
        static_cast<std::size_t>(num_channels_) * max_block_size, 0.0f);
    output_ptrs_.resize(num_channels_);
    for (int ch = 0; ch < num_channels_; ++ch)
        output_ptrs_[ch] = output_planar_.data() + ch * max_block_size;

    // Reserve once, then forbid growth — add() drops (counted) on overflow.
    midi_out_.reserve(kMidiEventCapacity, kSysexCapacity, kSysexPayloadCapacity);
    midi_out_.set_realtime_capacity_limit(true);
    return true;
}

void WamStage::prepare(double sample_rate, int block_size) {
    // Between render quanta (see WamProcessorBridge::prepare) — re-runs
    // prepare() and may resize the output buffer; never call from process().
    if (!processor_ || block_size <= 0) return;

    auto desc = processor_->descriptor();
    PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_buffer_size = block_size;
    ctx.input_channels = desc.default_input_channels();
    ctx.output_channels = desc.default_output_channels();
    processor_->prepare(ctx);
    processor_->on_non_realtime_tick();   // see WamProcessorBridge::prepare

    // Grow only when the block size actually increased.
    if (block_size > block_size_) {
        block_size_ = block_size;
        output_planar_.assign(
            static_cast<std::size_t>(num_channels_) * block_size_, 0.0f);
        for (int ch = 0; ch < num_channels_; ++ch)
            output_ptrs_[ch] = output_planar_.data() + ch * block_size_;
    }
}

void WamStage::run(const audio::BufferView<const float>& in_view,
                   midi::MidiBuffer& midi_in, const ProcessContext& ctx,
                   int num_frames) {
    if (!processor_) return;
    if (num_frames > block_size_) num_frames = block_size_;
    if (num_frames <= 0) return;

    // This stage's MIDI output is fresh each block.
    midi_out_.clear();
    midi_out_.clear_sysex();

    audio::BufferView<float> out_view(output_ptrs_.data(),
                                      static_cast<std::size_t>(num_channels_),
                                      static_cast<std::size_t>(num_frames));
    processor_->process(out_view, in_view, midi_in, midi_out_, ctx);
}

audio::BufferView<const float> WamStage::output_as_input(int num_frames) const {
    if (num_frames > block_size_) num_frames = block_size_;
    if (num_frames < 0) num_frames = 0;
    return audio::BufferView<const float>(
        const_cast<const float* const*>(output_ptrs_.data()),
        static_cast<std::size_t>(num_channels_),
        static_cast<std::size_t>(num_frames));
}

void WamStage::copy_output_into(float* const* outputs, int host_channels,
                                int num_frames) const {
    if (!outputs) return;
    if (num_frames > block_size_) num_frames = block_size_;
    if (num_frames <= 0) return;
    const int ch = (std::min)(host_channels, num_channels_);
    for (int c = 0; c < ch; ++c) {
        if (outputs[c])
            std::memcpy(outputs[c], output_ptrs_[c],
                        static_cast<std::size_t>(num_frames) * sizeof(float));
    }
}

std::vector<uint8_t> WamStage::get_state() const {
    return encode_stage_state(store_, processor_.get());
}

bool WamStage::set_state(const uint8_t* data, size_t size) {
    const bool ok = decode_stage_state(store_, processor_.get(), data, size);
    // Same contract as WamProcessorBridge::set_state: a restored state can name
    // a different derived source than the live one, and a wasm module has no
    // worker thread to notice. Mark dirty; the rack services it after the next
    // block (see WamStage::service_non_realtime).
    mark_non_realtime_dirty();
    return ok;
}

// ── WamChainBridge ──────────────────────────────────────────────────────

namespace {

// Parse a rack parameter id: "<stage>:<paramId>" or, for stage 0, plain
// "<paramId>". Non-throwing (audio-thread reachable): returns nullopt on any
// malformed input or out-of-range stage.
std::optional<std::pair<std::size_t, state::ParamID>>
parse_chain_id(const std::string& id, std::size_t num_stages) {
    if (num_stages == 0) return std::nullopt;
    const auto colon = id.find(':');
    std::size_t stage = 0;
    std::string param_part = id;
    if (colon != std::string::npos) {
        unsigned long long s = 0;
        const char* begin = id.data();
        const char* end = id.data() + colon;
        auto [ptr, ec] = std::from_chars(begin, end, s);
        if (ec != std::errc{} || ptr != end) return std::nullopt;
        stage = static_cast<std::size_t>(s);
        param_part = id.substr(colon + 1);
    }
    if (stage >= num_stages) return std::nullopt;
    auto pid = parse_param_id(param_part);
    if (!pid) return std::nullopt;
    return std::make_pair(stage, *pid);
}

} // namespace

WamChainBridge::WamChainBridge(ChainFactory factory) : factory_(factory) {}
WamChainBridge::~WamChainBridge() = default;

bool WamChainBridge::initialize(double sample_rate, int max_block_size) {
    auto processors = factory_ ? factory_()
                               : std::vector<std::unique_ptr<Processor>>{};
    if (processors.empty()) return false;

    sample_rate_ = sample_rate;
    block_size_ = max_block_size;

    stages_.clear();
    stages_.reserve(processors.size());
    for (auto& p : processors) {
        auto stage = std::make_unique<WamStage>();
        if (!stage->initialize(std::move(p), sample_rate, max_block_size))
            return false;
        stages_.push_back(std::move(stage));
    }

    // Host-facing input width = stage 0's bus width. Stage 0 renders directly
    // from the host's planar input pointers (no de-interleave scratch).
    num_channels_ = stages_.front()->num_channels();
    if (num_channels_ < 1) num_channels_ = 2;

    pending_midi_.reserve(kMidiEventCapacity, kSysexCapacity,
                          kSysexPayloadCapacity);
    pending_midi_.set_realtime_capacity_limit(true);

    constexpr std::size_t kRecordHeader = sizeof(int32_t) + sizeof(uint16_t);
    midi_out_scratch_.assign(
        kMidiEventCapacity * (kRecordHeader + 3)
            + kSysexCapacity * (kRecordHeader + kSysexPayloadCapacity),
        0);
    midi_out_bytes_ = 0;

    runtime::log_info("WAMv2: initialized rack of {} stages at {} Hz",
                      stages_.size(), sample_rate);
    return true;
}

void WamChainBridge::prepare(double sample_rate, int block_size) {
    // Between render quanta (see WamProcessorBridge::prepare); never from process().
    if (block_size <= 0) return;
    sample_rate_ = sample_rate;
    for (auto& st : stages_) st->prepare(sample_rate, block_size);
    // Track the largest block the stages are prepared for; process() clamps to
    // it. No input scratch to resize — stage 0 reads the host pointers directly.
    if (block_size > block_size_) block_size_ = block_size;
}

int WamChainBridge::latency_samples() const {
    int total = 0;
    for (const auto& st : stages_) total += st->latency_samples();
    return total;
}

void WamChainBridge::set_transport(bool is_playing, double bpm,
                                    double position_beats,
                                    double position_samples, int tsig_num,
                                    int tsig_den) noexcept {
    store_transport(transport_, is_playing, bpm, position_beats,
                    position_samples, tsig_num, tsig_den);
}

void WamChainBridge::process(const float* const* inputs, float* const* outputs,
                              int num_channels, int num_frames) {
    if (stages_.empty()) return;
    if (num_frames > block_size_) num_frames = block_size_;
    if (num_frames <= 0) return;

    const int host_ch = (std::min)(num_channels, num_channels_);

    // Stage 0 renders directly from the host's planar input pointers — no
    // de-interleave scratch (the rack is planar end-to-end).
    audio::BufferView<const float> stage0_in(
        inputs, static_cast<std::size_t>(host_ch),
        static_cast<std::size_t>(num_frames));

    ProcessContext ctx;
    ctx.sample_rate = sample_rate_;
    ctx.num_samples = num_frames;
    ctx.process_mode = ProcessMode::Realtime;
    ctx.render_speed_hint = RenderSpeedHint::Realtime;
    ctx.is_playing = transport_.is_playing;
    ctx.tempo_bpm = transport_.tempo_bpm;
    ctx.position_beats = transport_.position_beats;
    ctx.position_samples = transport_.position_samples;
    ctx.time_sig_numerator = transport_.time_sig_numerator;
    ctx.time_sig_denominator = transport_.time_sig_denominator;
    if (transport_.valid) {
        ctx.transport_validity.set(TransportField::Playing);
        ctx.transport_validity.set(TransportField::Tempo);
        ctx.transport_validity.set(TransportField::BeatPosition);
        ctx.transport_validity.set(TransportField::SamplePosition);
        ctx.transport_validity.set(TransportField::TimeSignature);
    }

    // One-shot rack reset: every stage sees reset_requested for exactly this
    // block, then the flag clears.
    if (reset_pending_) {
        ctx.reset_requested = true;
        reset_pending_ = false;
    }

    // Stage 0 consumes the host's inbound MIDI + audio.
    stages_[0]->run(stage0_in, pending_midi_, ctx, num_frames);

    // Each later stage takes the PREVIOUS stage's output buffer as its audio
    // input and the previous stage's midi_out as its midi_in — handed BY
    // REFERENCE in the same wasm memory. No JS marshalling, no copy, and no
    // sample-offset rewriting: an event at offset 37 out of stage i is still at
    // offset 37 going into stage i+1, within THIS block.
    for (std::size_t i = 1; i < stages_.size(); ++i) {
        auto in_view = stages_[i - 1]->output_as_input(num_frames);
        stages_[i]->run(in_view, stages_[i - 1]->midi_out(), ctx, num_frames);
    }

    // The rack's drainable MIDI output is the LAST stage's midi_out.
    const auto& last = *stages_.back();
    serialize_midi_out(midi_out_scratch_, midi_out_bytes_, last.midi_out());

    // The block consumed its inbound events.
    pending_midi_.clear();
    pending_midi_.clear_sysex();

    // The last stage's planar audio is copied into the host's planar output.
    last.copy_output_into(outputs, num_channels, num_frames);

    // COALESCED non-realtime pass for every stage, AFTER the audio is produced —
    // at most one reconcile per stage per block, however many control messages
    // arrived (see WamStage::service_non_realtime).
    for (auto& stage : stages_) stage->service_non_realtime();
}

std::string WamChainBridge::parameters_json() const {
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (std::size_t s = 0; s < stages_.size(); ++s) {
        for (const auto& p : stages_[s]->store().all_params()) {
            if (!first) ss << ",";
            first = false;
            WamParamInfo info = make_param_info(p);
            info.id = std::to_string(s) + ":" + std::to_string(p.id);
            append_param_json(ss, info, static_cast<int>(s));
        }
    }
    ss << "]";
    return ss.str();
}

uint32_t WamChainBridge::param_epoch() const {
    uint32_t sum = 0;
    for (const auto& stage : stages_) sum += stage->param_epoch();
    return sum;   // wraps like any counter; hosts compare for inequality only
}

int WamChainBridge::read_param_values(float* dst, int capacity) const {
    int written = 0, total = 0;
    for (const auto& stage : stages_) {
        const int remaining = (std::max)(0, capacity - written);
        float* cursor = (dst && remaining > 0) ? dst + written : nullptr;
        const int count = stage->read_param_values(cursor, remaining);
        written += (std::min)(count, remaining);
        total += count;
    }
    return total;
}

float WamChainBridge::get_parameter_value(const std::string& id) const {
    auto parsed = parse_chain_id(id, stages_.size());
    if (!parsed) return 0.0f;
    return stages_[parsed->first]->get_param(parsed->second);
}

void WamChainBridge::set_parameter_value(const std::string& id, float value) {
    auto parsed = parse_chain_id(id, stages_.size());
    if (!parsed) return;
    stages_[parsed->first]->set_param(parsed->second, value);
}

void WamChainBridge::schedule_midi(uint8_t status, uint8_t data1, uint8_t data2,
                                    int sample_offset) {
    schedule_short_midi(pending_midi_, status, data1, data2, sample_offset);
}

bool WamChainBridge::schedule_sysex(const uint8_t* data, int size,
                                     int sample_offset) {
    return schedule_sysex_into(pending_midi_, data, size, sample_offset);
}

int WamChainBridge::drain_midi_out(uint8_t* dst, int cap) const {
    return drain_records(midi_out_scratch_, midi_out_bytes_, dst, cap);
}

std::vector<uint8_t> WamChainBridge::get_state() const {
    // "PWR1" [u32 stage_count] ( [u32 len][per-stage plugin_state_io blob] )*
    std::vector<uint8_t> out;
    out.insert(out.end(), kRackStateMagic.begin(), kRackStateMagic.end());
    append_u32(out, static_cast<uint32_t>(stages_.size()));
    for (const auto& stage : stages_) {
        auto blob = stage->get_state();
        append_u32(out, static_cast<uint32_t>(blob.size()));
        out.insert(out.end(), blob.begin(), blob.end());
    }
    return out;
}

bool WamChainBridge::set_state(const uint8_t* data, size_t size) {
    if (!data || size == 0 || stages_.empty()) return false;

    // A single-plugin blob (bare StateStore or a plugin_state_io "PLST"
    // envelope) loads into stage 0. Other stages keep their initialized defaults.
    if (!has_rack_state_magic(data, size)) {
        return stages_[0]->set_state(data, size);
    }

    size_t pos = kRackStateMagic.size();
    uint32_t stage_count = 0;
    if (!read_u32(data, size, pos, stage_count)) return false;

    bool ok = true;
    for (uint32_t s = 0; s < stage_count; ++s) {
        uint32_t len = 0;
        if (!read_u32(data, size, pos, len)) return false;
        if (len > size - pos) return false;   // wrap-safe (pos <= size holds)
        if (s < stages_.size())
            ok = stages_[s]->set_state(data + pos, len) && ok;
        pos += len;
    }
    return ok;
}

std::string WamChainBridge::descriptor_json() const {
    if (stages_.empty()) return "{}";

    const auto first = stages_.front()->plugin_descriptor();
    const auto last = stages_.back()->plugin_descriptor();

    // Populate the ONE descriptor POD instead of hand-emitting a second JSON
    // object: the rack is just a WamDescriptorData whose endpoints come from the
    // first/last stages, with a non-empty `stages` list. to_json() then produces
    // exactly the bytes this used to spell out (the single-plugin defaults for
    // apiVersion / automation / MPE are the same values it hardcoded).
    WamDescriptorData d;
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        if (i) d.name += " \xe2\x86\x92 ";   // U+2192, UTF-8; escaper passes through
        d.name += stages_[i]->plugin_descriptor().name;
        d.stages.push_back(stages_[i]->plugin_descriptor().name);
    }
    d.vendor = first.manufacturer;
    d.version = first.version;
    d.is_instrument = (last.category == PluginCategory::Instrument);
    d.has_audio_input = first.default_input_channels() > 0;
    d.has_audio_output = last.default_output_channels() > 0;
    d.has_midi_input = first.accepts_midi;
    d.has_midi_output = last.produces_midi;
    d.tail_samples = last.tail_samples;
    d.latency_samples = 0;
    for (const auto& st : stages_) d.latency_samples += st->latency_samples();
    return d.to_json();
}

} // namespace pulp::format::wam

// Emscripten exports are provided by per-plugin entry point files
// (e.g., pulp_gain_wasm.cpp) which create a WamProcessorBridge
// and expose the wam_init/wam_process/wam_set_param/etc. C exports.
