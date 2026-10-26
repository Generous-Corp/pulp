#include <pulp/format/descriptor_validation.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/runtime/exceptions.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <unordered_set>

namespace pulp::format {

bool ProcessorNode::prepare(const PrepareContext& context) {
    if (processor_ == nullptr)
        return false;
    prepared_ = false;
    PULP_TRY {
        const auto descriptor = processor_->descriptor();
        capabilities_ = descriptor.effective_capabilities();
        // Reserve one inline bus slot for the required output.  The routed
        // adapter cannot represent all kMaxBuses inputs and an output in the
        // same ProcessBlock otherwise.
        input_bus_count_ = std::min(descriptor.input_buses.size(), input_roles_.size() - 1);
        for (std::size_t index = 0; index < input_bus_count_; ++index) {
            auto role = descriptor.input_buses[index].role;
            // Preserve compatibility with the legacy three-field descriptor:
            // an optional secondary input was historically a sidechain and
            // had no explicit role field.
            if (index > 0 && descriptor.input_buses[index].optional && role == BusRole::Main)
                role = BusRole::Sidechain;
            input_roles_[index] = role;
            input_channels_[index] = descriptor.input_buses[index].default_channels;
            input_optional_[index] = descriptor.input_buses[index].optional;
        }
        processor_->prepare(context);
        // Pre-size the discard MIDI sinks so the per-block clear() the bridge runs
        // on the unused fallback buffers never reallocates on the RT thread.
        const int midi_hint = context.resource_limits.max_midi_events;
        const auto reserve = static_cast<std::size_t>(midi_hint > 0 ? midi_hint : 1);
        scratch_.fallback_midi_in.reserve_events(reserve);
        scratch_.fallback_midi_out.reserve_events(reserve);
    }
    PULP_CATCH_ALL {
        capabilities_ = {};
        input_bus_count_ = 0;
        return false;
    }
    prepared_ = true;
    return true;
}

bool ProcessorNode::release() noexcept {
    prepared_ = false;
    capabilities_ = {};
    input_bus_count_ = 0;
    if (processor_ == nullptr)
        return false;
    PULP_TRY {
        processor_->release();
        return true;
    }
    PULP_CATCH_ALL {
        return false;
    }
}

GraphRuntimeNodeBinding ProcessorNode::binding(graph::NodeId node_id, bool required) noexcept {
    GraphRuntimeNodeBinding result;
    result.node_id = node_id;
    if (!prepared_) {
        result.required = required;
        return result;
    }
    result.process = &ProcessorNode::process_binding;
    result.user_data = this;
    result.required = required;
    result.audio_rate_modulation_delivery =
        capabilities_.consumes_audio_rate_modulations
            ? AudioRateModulationDelivery::DenseViews
            : AudioRateModulationDelivery::LegacyParameterEvents;
    return result;
}

bool ProcessorNode::process_binding(ProcessBlock& block, const GraphRuntimeNodeProcessContext& ctx,
                                    void* user_data) noexcept {
    auto* self = static_cast<ProcessorNode*>(user_data);
    if (self == nullptr || self->processor_ == nullptr)
        return false;
    // This binding only implements the routed contract: it reads the gathered
    // per-node input and writes the per-node output. The shared-block path does
    // not populate those views, so refuse it rather than silently misread the
    // block's buses.
    if (!ctx.routed)
        return false;

    // The node's bus set aliases the executor's routed main view and any
    // host-provided optional input views. BusBufferSet stores its buses inline
    // and BufferView only holds host-owned channel pointers, so this aliases
    // without copying samples and stays allocation-free. A node is never run
    // on two threads at once, so its set is reused rather than rebuilt.
    BusBufferSet& local_buses = self->buses_;
    local_buses.clear();
    const auto main_role = self->input_bus_count_ > 0 ? self->input_roles_[0] : BusRole::Main;
    if (!local_buses.add_input("main", ctx.node_inputs, main_role, true))
        return false;
    for (std::size_t bus = 1; bus < self->input_bus_count_; ++bus) {
        audio::BufferView<const float> route_view;
        bool active = false;
        if (self->input_provider_ &&
            self->input_provider_.read(self->input_provider_.context, bus, block.frame_count,
                                       route_view) &&
            route_view.num_samples() == block.frame_count &&
            route_view.num_channels() ==
                static_cast<std::size_t>(std::max(0, self->input_channels_[bus]))) {
            active = true;
        } else {
            route_view = {};
        }
        if (!local_buses.add_input("aux", route_view, self->input_roles_[bus], active))
            return false;
    }
    if (!local_buses.add_output("main", ctx.node_outputs, BusRole::Main))
        return false;

    ProcessBlock local_block;
    local_block.mode = block.mode;
    local_block.flags = block.flags;
    local_block.sample_rate = block.sample_rate;
    local_block.frame_count = block.frame_count;
    local_block.render_speed = block.render_speed;
    local_block.block_index = block.block_index;
    local_block.transport = block.transport;
    local_block.buses = &local_buses;
    EventBlock events;
    events.parameter_events = ctx.node_param_events;
    events.midi_in = ctx.node_midi_in;
    events.midi_out = ctx.node_midi_out;
    events.audio_rate_modulations = ctx.node_audio_rate_modulations;
    local_block.events = &events;

    if (self->capabilities_.consumes_audio_rate_modulations) {
        // process_processor_block() validates the legacy path itself.
        if (!local_block.validate())
            return false;
        PULP_TRY {
            runtime::ScopedNoAlloc no_alloc_guard;
            if (self->processor_->process_block(local_block))
                return true;
        }
        PULP_CATCH_ALL {}
        auto outputs = ctx.node_outputs;
        for (std::size_t channel = 0; channel < outputs.num_channels(); ++channel) {
            std::fill_n(outputs.channel_ptr(channel), block.frame_count, 0.0f);
        }
        return false;
    }
    return process_processor_block(*self->processor_, local_block, self->scratch_);
}

std::shared_ptr<ProcessorNodeInstance>
ProcessorNodeInstance::create(std::unique_ptr<Processor> processor) noexcept {
    if (processor == nullptr)
        return {};
    PULP_TRY {
        auto instance =
            std::shared_ptr<ProcessorNodeInstance>(new ProcessorNodeInstance(std::move(processor)));
        instance->descriptor_ = instance->processor_->descriptor();
        if (!descriptor_is_valid(validate_descriptor(instance->descriptor_)))
            return {};

        instance->processor_->set_state_store(&instance->state_store_);
        instance->processor_->define_parameters(instance->state_store_);
        const auto params = instance->state_store_.all_params();
        std::unordered_set<state::ParamID> ids;
        ids.reserve(params.size());
        instance->parameter_catalog_.reserve(params.size());
        for (const auto& param : params) {
            if (!ids.insert(param.id).second)
                return {};
            instance->parameter_catalog_.push_back(param);
        }
        return instance;
    }
    PULP_CATCH_ALL {
        return {};
    }
}

ProcessorNodeInstance::~ProcessorNodeInstance() {
    if (!prepared_ || processor_ == nullptr)
        return;
    (void)adapter_.release();
}

bool ProcessorNodeInstance::prepare(const PrepareContext& context) noexcept {
    if (prepared_) {
        prepared_ = false;
        if (!adapter_.release())
            return false;
    }
    PULP_TRY {
        if (!adapter_.prepare(context)) {
            (void)adapter_.release();
            return false;
        }
    }
    PULP_CATCH_ALL {
        (void)adapter_.release();
        return false;
    }
    prepared_ = true;
    return true;
}

bool ProcessorNodeInstance::release() noexcept {
    if (!prepared_)
        return true;
    prepared_ = false;
    return adapter_.release();
}

const state::ParamInfo* ProcessorNodeInstance::parameter(state::ParamID id) const noexcept {
    const auto found = std::find_if(parameter_catalog_.begin(), parameter_catalog_.end(),
                                    [id](const state::ParamInfo& param) { return param.id == id; });
    return found == parameter_catalog_.end() ? nullptr : &*found;
}

} // namespace pulp::format
