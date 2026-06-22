#include <pulp/format/native_core_processor.hpp>

#include <pulp/native_components/native_core.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::format {

namespace {

std::string to_string(const char* p, std::size_t n) {
    return (p != nullptr && n > 0) ? std::string(p, n) : std::string();
}

}  // namespace

NativeCoreProcessor::NativeCoreProcessor(const pulp_native_core_v1* core) {
    if (!pulp::native_components::is_compatible(core)) {
        return;  // inert: instance_ stays null, descriptor() returns placeholder
    }
    core_ = core;
    host_.size = sizeof(host_);
    host_.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    host_.host_context = this;
    host_.alloc = &NativeCoreProcessor::host_alloc;
    host_.free = &NativeCoreProcessor::host_free;
    host_.log = &NativeCoreProcessor::host_log;
    host_.push_midi_out = &NativeCoreProcessor::host_push_midi_out;
    host_.notify_latency_changed = &NativeCoreProcessor::host_notify_latency_changed;

    pulp_native_instance* inst = nullptr;
    if (core_->create(&host_, &inst) == PULP_NATIVE_OK) {
        instance_ = inst;
    }
}

NativeCoreProcessor::~NativeCoreProcessor() {
    if (core_ != nullptr && instance_ != nullptr) {
        core_->destroy(instance_);
        instance_ = nullptr;
    }
}

void* NativeCoreProcessor::host_alloc(void*, std::size_t bytes) {
    pulp_rt_trap_if_no_alloc_scope(1, bytes);
    return std::malloc(bytes);
}
void NativeCoreProcessor::host_free(void*, void* ptr) { std::free(ptr); }
void NativeCoreProcessor::host_log(void*, int32_t, const char*, std::size_t) {
    // Non-RT diagnostic sink; intentionally a no-op for the SDK adapter.
}
int32_t NativeCoreProcessor::host_push_midi_out(void*,
                                                const pulp_native_midi_event_v1*) {
    // MIDI-out bridging is not implemented here because MidiBuffer::add allocates.
    return 0;
}
void NativeCoreProcessor::host_notify_latency_changed(void* ctx) {
    auto* self = static_cast<NativeCoreProcessor*>(ctx);
    self->latency_changed_.store(true, std::memory_order_relaxed);
}

PluginDescriptor NativeCoreProcessor::descriptor() const {
    PluginDescriptor d;
    if (instance_ == nullptr) {
        d.name = "Native Core (unavailable)";
        return d;
    }
    const pulp_native_descriptor_v1* nd = core_->descriptor();
    d.name = to_string(nd->id, nd->id_len);
    if (nd->name != nullptr && nd->name_len > 0) {
        d.name = to_string(nd->name, nd->name_len);
    }
    d.manufacturer = to_string(nd->vendor, nd->vendor_len);
    d.version = std::to_string(nd->plugin_version);
    d.category = (nd->capabilities & PULP_NATIVE_CAP_IS_INSTRUMENT)
                     ? PluginCategory::Instrument
                     : PluginCategory::Effect;
    d.accepts_midi = (nd->capabilities & PULP_NATIVE_CAP_MIDI_INPUT) != 0;
    d.produces_midi = (nd->capabilities & PULP_NATIVE_CAP_MIDI_OUTPUT) != 0;
    d.supports_mpe = (nd->capabilities & PULP_NATIVE_CAP_MPE) != 0;
    d.supports_ump = (nd->capabilities & PULP_NATIVE_CAP_UMP) != 0;
    d.tail_samples = static_cast<int>(nd->tail_frames);
    // Bus counts come from the descriptor; channel counts default to stereo until
    // bus-layout renegotiation is wired.
    d.input_buses.clear();
    for (uint32_t i = 0; i < nd->default_input_bus_count; ++i) {
        d.input_buses.push_back({"Input " + std::to_string(i + 1), 2, false});
    }
    d.output_buses.clear();
    for (uint32_t i = 0; i < nd->default_output_bus_count; ++i) {
        d.output_buses.push_back({"Output " + std::to_string(i + 1), 2, false});
    }
    if (d.input_buses.empty()) d.input_buses.push_back({"Main In", 2, false});
    if (d.output_buses.empty()) d.output_buses.push_back({"Main Out", 2, false});
    return d;
}

void NativeCoreProcessor::define_parameters(state::StateStore& store) {
    if (instance_ == nullptr) return;
    uint32_t count = 0;
    const pulp_native_param_v1* params = core_->parameters(&count);
    paramid_to_hash_.clear();
    paramid_to_hash_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const pulp_native_param_v1& p = params[i];
        state::ParamInfo info;
        info.id = i;  // ParamID == registration index
        info.name = (p.name != nullptr && p.name_len > 0)
                        ? to_string(p.name, p.name_len)
                        : to_string(p.id, p.id_len);
        info.range.min = static_cast<float>(p.min_value);
        info.range.max = static_cast<float>(p.max_value);
        info.range.default_value = static_cast<float>(p.default_value);
        if ((p.flags & PULP_NATIVE_PARAM_STEPPED) && p.step_count > 0 &&
            p.max_value > p.min_value) {
            info.range.step =
                static_cast<float>((p.max_value - p.min_value) / p.step_count);
        }
        store.add_parameter(info);
        paramid_to_hash_.push_back(p.id_hash);
    }
}

void NativeCoreProcessor::prepare(const PrepareContext& context) {
    if (instance_ == nullptr) return;
    prepare_ctx_ = context;

    const int in_ch = context.input_channels > 0 ? context.input_channels : 2;
    const int out_ch = context.output_channels > 0 ? context.output_channels : 2;
    in_ptrs_.assign(static_cast<std::size_t>(in_ch), nullptr);
    out_ptrs_.assign(static_cast<std::size_t>(out_ch), nullptr);
    in_buses_.assign(1, pulp_native_audio_bus_v1{});
    out_buses_.assign(1, pulp_native_audio_bus_v1{});
    event_scratch_.assign(state::ParameterEventQueue::kCapacity,
                          pulp_native_param_event_v1{});

    // Non-RT: build the bus layout for prepare().
    std::vector<uint32_t> in_counts(1, static_cast<uint32_t>(in_ch));
    std::vector<uint32_t> out_counts(1, static_cast<uint32_t>(out_ch));
    pulp_native_bus_layout_v1 layout{};
    layout.size = sizeof(layout);
    layout.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    layout.input_channel_counts = in_counts.data();
    layout.input_bus_count = 1;
    layout.output_channel_counts = out_counts.data();
    layout.output_bus_count = 1;

    pulp_native_prepare_v1 cfg{};
    cfg.size = sizeof(cfg);
    cfg.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    cfg.sample_rate = context.sample_rate;
    cfg.max_block_size = static_cast<uint32_t>(context.max_buffer_size);
    cfg.layout = layout;

    if (core_->prepare(instance_, &cfg) == PULP_NATIVE_OK) {
        core_->resume(instance_);
        prepared_ = true;
    }
}

void NativeCoreProcessor::release() {
    if (instance_ == nullptr) return;
    core_->suspend(instance_);
    core_->release(instance_);
    prepared_ = false;
}

void NativeCoreProcessor::suspend() {
    if (instance_ != nullptr) core_->suspend(instance_);
}
void NativeCoreProcessor::resume() {
    if (instance_ != nullptr) core_->resume(instance_);
}

void NativeCoreProcessor::process(audio::BufferView<float>& audio_output,
                                  const audio::BufferView<const float>& audio_input,
                                  midi::MidiBuffer& /*midi_in*/,
                                  midi::MidiBuffer& /*midi_out*/,
                                  const ProcessContext& context) {
    pulp::runtime::ScopedNoAlloc no_alloc_guard;

    if (instance_ == nullptr || !prepared_) {
        audio_output.clear();
        return;
    }

    const std::size_t in_ch =
        std::min(audio_input.num_channels(), in_ptrs_.size());
    const std::size_t out_ch =
        std::min(audio_output.num_channels(), out_ptrs_.size());
    for (std::size_t c = 0; c < in_ch; ++c) {
        // Inputs are read-only per contract; the POD bus type is float* const*.
        in_ptrs_[c] = const_cast<float*>(audio_input.channel_ptr(c));
    }
    for (std::size_t c = 0; c < out_ch; ++c) {
        out_ptrs_[c] = audio_output.channel_ptr(c);
    }

    in_buses_[0] = {sizeof(pulp_native_audio_bus_v1), static_cast<uint32_t>(in_ch),
                    in_ptrs_.data()};
    out_buses_[0] = {sizeof(pulp_native_audio_bus_v1),
                     static_cast<uint32_t>(out_ch), out_ptrs_.data()};

    pulp_native_audio_io_v1 io{};
    io.size = sizeof(io);
    io.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    io.frame_count = static_cast<uint32_t>(audio_output.num_samples());
    io.input_bus_count = 1;
    io.output_bus_count = 1;
    io.sidechain_bus_count = 0;
    io.inputs = in_buses_.data();
    io.outputs = out_buses_.data();
    io.sidechains = nullptr;

    // Translate the borrowed Pulp parameter-event queue (automation, plain
    // domain, sorted, ramped) into the native event view.
    pulp_native_param_event_view_v1 view{};
    view.size = sizeof(view);
    view.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    const state::ParameterEventQueue* queue = param_events();
    if (queue != nullptr) {
        uint32_t n = 0;
        for (const state::ParameterEvent& e : queue->events()) {
            if (n >= event_scratch_.size()) break;
            const uint64_t hash =
                (e.param_id < paramid_to_hash_.size())
                    ? paramid_to_hash_[e.param_id]
                    : 0;
            event_scratch_[n] = {hash, static_cast<double>(e.value),
                                 static_cast<uint32_t>(e.sample_offset),
                                 static_cast<uint32_t>(e.ramp_duration_sample_frames),
                                 PULP_NATIVE_EVENT_AUTOMATION, 0};
            ++n;
        }
        view.events = event_scratch_.data();
        view.count = n;
        view.capacity = static_cast<uint32_t>(event_scratch_.size());
        view.overflowed = queue->overflowed() ? 1u : 0u;
    } else {
        view.events = nullptr;  // distinct from present-but-empty
    }

    pulp_native_process_context_v1 pctx{};
    pctx.size = sizeof(pctx);
    pctx.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    pctx.sample_rate = context.sample_rate;
    pctx.tempo_bpm = context.tempo_bpm;
    pctx.ppq_position = context.position_beats;
    pctx.playhead_frames = context.position_samples;
    pctx.is_playing = context.is_playing ? 1u : 0u;
    pctx.is_looping = context.is_looping ? 1u : 0u;
    pctx.time_sig_numerator = static_cast<uint32_t>(context.time_sig_numerator);
    pctx.time_sig_denominator = static_cast<uint32_t>(context.time_sig_denominator);

    pulp_native_process_v1 proc{};
    proc.size = sizeof(proc);
    proc.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    proc.audio = &io;
    proc.params = &view;
    proc.midi = nullptr;  // MIDI bridging is not implemented here.
    proc.context = &pctx;

    if (core_->process(instance_, &proc) != PULP_NATIVE_OK) {
        audio_output.clear();  // safe silence on core failure
    }
}

std::vector<uint8_t> NativeCoreProcessor::serialize_plugin_state() const {
    if (instance_ == nullptr) return {};
    pulp_native_state_out_v1 out{};
    out.size = sizeof(out);
    out.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    if (core_->save_state(instance_, &out) != PULP_NATIVE_OK || out.bytes == nullptr) {
        return {};
    }
    std::vector<uint8_t> bytes(out.bytes, out.bytes + out.byte_len);
    core_->free_state(instance_, &out);
    return bytes;
}

bool NativeCoreProcessor::deserialize_plugin_state(std::span<const uint8_t> data) {
    if (instance_ == nullptr) return false;
    pulp_native_state_span_v1 span{};
    span.size = sizeof(span);
    span.abi_version = PULP_NATIVE_CORE_ABI_VERSION;
    span.bytes = data.data();
    span.byte_len = data.size();
    const pulp_native_status st = core_->load_state(instance_, &span);
    return st == PULP_NATIVE_OK;
}

int NativeCoreProcessor::latency_samples() const {
    if (instance_ == nullptr) return 0;
    return static_cast<int>(core_->report_latency(instance_));
}

std::optional<std::string> NativeCoreProcessor::editor_command(
    std::string_view request_json) {
    if (instance_ == nullptr || core_->editor_command == nullptr) {
        return std::nullopt;
    }
    uint8_t* reply = nullptr;
    std::size_t reply_len = 0;
    const pulp_native_status st = core_->editor_command(
        instance_, reinterpret_cast<const uint8_t*>(request_json.data()),
        request_json.size(), &reply, &reply_len);
    if (st != PULP_NATIVE_OK) {
        if (reply != nullptr) core_->free_editor_reply(instance_, reply, reply_len);
        return std::nullopt;  // unsupported / rejected — never unwinds
    }
    std::string out;
    if (reply != nullptr && reply_len > 0) {
        out.assign(reinterpret_cast<const char*>(reply), reply_len);
    }
    if (reply != nullptr) core_->free_editor_reply(instance_, reply, reply_len);
    return out;
}

}  // namespace pulp::format
