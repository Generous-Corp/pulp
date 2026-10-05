// Signal Graph implementation.
//
// Audio-thread reads happen against a CompiledGraph snapshot published via an
// atomic raw pointer. UI-thread topology mutations (add_*, connect*,
// disconnect, remove_node) invalidate the snapshot; prepare() rebuilds and
// publishes a fresh snapshot. Control-thread shared_ptr owners keep retired
// snapshots alive until active process readers drain, avoiding libstdc++'s
// lock-taking atomic shared_ptr path in process().
// See signal_graph.hpp for the mutation protocol details.

#include "signal_graph_internal.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <pulp/format/processor.hpp>
#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_execution_snapshot.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/runtime/log.hpp>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {

bool SampleKernelDescriptor::is_valid_registration() const noexcept {
    const bool alignment_valid =
        state_alignment != 0 && (state_alignment & (state_alignment - 1)) == 0;
    if (abi_version != kAbiVersion || type_id.empty() || version <= 0 ||
        scope != SampleKernelScope::RegionOnly || !alignment_valid || latency_samples != 0 ||
        metadata.category.empty()) {
        return false;
    }
    switch (authored_config_kind) {
    case SampleKernelConfigKind::None:
    case SampleKernelConfigKind::BoundaryIndex:
    case SampleKernelConfigKind::FiniteConstant:
    case SampleKernelConfigKind::PromotedParameterId:
        break;
    case SampleKernelConfigKind::Invalid:
        return false;
    default:
        return false;
    }
    if (metadata.has_value_range &&
        (!std::isfinite(metadata.minimum_value) || !std::isfinite(metadata.maximum_value) ||
         metadata.minimum_value > metadata.maximum_value)) {
        return false;
    }

    const bool has_lifecycle = construct || reset || destroy;
    if (state_size == 0) {
        if (state_alignment != 1 || has_lifecycle)
            return false;
    } else if (!construct || !reset || !destroy) {
        return false;
    }

    switch (causality) {
    case SampleKernelCausality::Combinational:
        return process != nullptr && delay_publish == nullptr && delay_commit == nullptr;
    case SampleKernelCausality::OneSampleDelay:
        return state_size != 0 && process == nullptr && delay_publish != nullptr &&
               delay_commit != nullptr;
    }
    return false;
}

// Identity token for exact-parameter ingress ownership. Empty by design: claims
// are keyed on the shared_ptr's control-block identity and scoped by its
// lifetime. Defined here so the signal graph — not the timeline binding that
// claims nodes — owns the type.
namespace detail {
struct ExactParameterIngressOwner {};

std::shared_ptr<ExactParameterIngressOwner> make_exact_parameter_ingress_owner() {
    return std::make_shared<ExactParameterIngressOwner>();
}
} // namespace detail

namespace {

std::uint64_t next_nonzero_mailbox_sequence(
    std::atomic<std::uint64_t>& counter) noexcept {
    auto current = counter.load(std::memory_order_relaxed);
    for (;;) {
        const auto next = current == std::numeric_limits<std::uint64_t>::max()
                              ? std::uint64_t{1}
                              : current + 1;
        // The mailbox's single-writer contract means contention is not expected;
        // CAS keeps the test seam and any accidental observer from losing an
        // update. Relaxed is sufficient because this atomic carries identity
        // only; TripleBuffer's release/acquire flag publishes the payload.
        if (counter.compare_exchange_weak(current, next,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
            return next;
        }
    }
}

bool parameter_allows_modulation(const HostParamInfo& p,
                                 uint32_t param_id,
                                 state::ParamRate required_rate,
                                 bool require_modulatable = false) {
    return p.id == param_id
        && p.rate == required_rate
        && p.flags.automatable
        && (!require_modulatable || p.flags.modulatable)
        && !p.flags.read_only
        && !p.flags.stepped;
}

bool has_input_port(const GraphNode& node, PortIndex port) {
    return node.num_input_ports > 0
        && port < static_cast<PortIndex>(node.num_input_ports);
}

bool has_output_port(const GraphNode& node, PortIndex port) {
    return node.num_output_ports > 0
        && port < static_cast<PortIndex>(node.num_output_ports);
}

std::size_t saturating_add(std::size_t a, std::size_t b) {
    const auto max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

std::size_t saturating_mul(std::size_t a, std::size_t b) {
    const auto max = std::numeric_limits<std::size_t>::max();
    if (a == 0 || b == 0) return 0;
    return a > max / b ? max : a * b;
}

std::uint64_t saturating_add_u64(std::uint64_t a, std::uint64_t b) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return b > max - a ? max : a + b;
}

std::uint64_t saturating_mul_u64(std::uint64_t a, std::uint64_t b) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    if (a == 0 || b == 0) return 0;
    return a > max / b ? max : a * b;
}

state::ModulationMixMode modulation_mix_for(AutomationMix mix) {
    switch (mix) {
        case AutomationMix::Replace: return state::ModulationMixMode::Replace;
        case AutomationMix::Add: return state::ModulationMixMode::Add;
    }
    return state::ModulationMixMode::Add;
}

template <typename T, typename GetId>
T* find_by_id(std::vector<T>& entries, uint32_t id, GetId get_id) {
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const T& entry) { return get_id(entry) == id; });
    return it == entries.end() ? nullptr : &*it;
}

bool custom_type_matches_node_shape(const CustomNodeType& type,
                                    const GraphNode& node) {
    return type.num_input_ports == node.num_input_ports
        && type.num_output_ports == node.num_output_ports;
}


bool metadata_equal(const SampleKernelMetadata& lhs, const SampleKernelMetadata& rhs) noexcept {
    return lhs.category == rhs.category && lhs.parameter == rhs.parameter &&
           lhs.units == rhs.units && lhs.minimum_value == rhs.minimum_value &&
           lhs.maximum_value == rhs.maximum_value && lhs.has_value_range == rhs.has_value_range &&
           lhs.capability_flags == rhs.capability_flags;
}

bool sample_descriptors_equal(const SampleKernelDescriptor& lhs,
                              const SampleKernelDescriptor& rhs) noexcept {
    return lhs.abi_version == rhs.abi_version && lhs.type_id == rhs.type_id &&
           lhs.version == rhs.version && lhs.num_input_ports == rhs.num_input_ports &&
           lhs.num_output_ports == rhs.num_output_ports && lhs.causality == rhs.causality &&
           lhs.scope == rhs.scope && lhs.authored_config_kind == rhs.authored_config_kind &&
           lhs.state_size == rhs.state_size && lhs.state_alignment == rhs.state_alignment &&
           lhs.construct == rhs.construct && lhs.reset == rhs.reset && lhs.destroy == rhs.destroy &&
           lhs.process == rhs.process && lhs.delay_publish == rhs.delay_publish &&
           lhs.delay_commit == rhs.delay_commit && lhs.latency_samples == rhs.latency_samples &&
           metadata_equal(lhs.metadata, rhs.metadata);
}

template <class R, class... Args>
bool same_function(const std::function<R(Args...)>& lhs,
                   const std::function<R(Args...)>& rhs) noexcept {
    if (static_cast<bool>(lhs) != static_cast<bool>(rhs))
        return false;
    if (!lhs)
        return true;
    using FunctionPointer = R (*)(Args...);
    const auto* lhs_target = lhs.template target<FunctionPointer>();
    const auto* rhs_target = rhs.template target<FunctionPointer>();
    return lhs_target != nullptr && rhs_target != nullptr && *lhs_target == *rhs_target;
}

bool custom_types_equal_for_idempotency(const CustomNodeType& lhs,
                                        const CustomNodeType& rhs) noexcept {
    if (lhs.type_id != rhs.type_id || lhs.version != rhs.version ||
        lhs.num_input_ports != rhs.num_input_ports ||
        lhs.num_output_ports != rhs.num_output_ports || lhs.default_name != rhs.default_name ||
        lhs.lowerable != rhs.lowerable || lhs.baked_params.size() != rhs.baked_params.size()) {
        return false;
    }
    // std::function erases arbitrary functor state, so only empty callbacks and
    // identical raw function-pointer targets can be proven equal. Conservatively
    // refuse to call captured or otherwise opaque functors idempotent.
    if (!same_function(lhs.process, rhs.process) || !same_function(lhs.create, rhs.create) ||
        !same_function(lhs.destroy, rhs.destroy) || !same_function(lhs.prepare, rhs.prepare) ||
        !same_function(lhs.release, rhs.release) || !same_function(lhs.reset, rhs.reset) ||
        !same_function(lhs.process_instance, rhs.process_instance) ||
        !same_function(lhs.save_state, rhs.save_state) ||
        !same_function(lhs.load_state, rhs.load_state) ||
        !same_function(lhs.process_transport, rhs.process_transport) ||
        !same_function(lhs.process_instance_transport, rhs.process_instance_transport) ||
        !same_function(lhs.process_instance_baked_param, rhs.process_instance_baked_param) ||
        !same_function(lhs.latency_samples, rhs.latency_samples) ||
        !same_function(lhs.latency_samples_for_block, rhs.latency_samples_for_block) ||
        !same_function(lhs.process_events, rhs.process_events) ||
        !same_function(lhs.process_instance_events, rhs.process_instance_events)) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.baked_params.size(); ++i) {
        const auto& a = lhs.baked_params[i];
        const auto& b = rhs.baked_params[i];
        if (a.id != b.id || a.min_value != b.min_value || a.max_value != b.max_value ||
            a.default_value != b.default_value) {
            return false;
        }
    }
    return true;
}

bool scalar_identity_matches(const CustomNodeType& type,
                             const SampleKernelDescriptor& descriptor) noexcept {
    return type.type_id == descriptor.type_id && type.version == descriptor.version &&
           type.num_input_ports >= 0 && type.num_output_ports >= 0 &&
           static_cast<std::uint32_t>(type.num_input_ports) == descriptor.num_input_ports &&
           static_cast<std::uint32_t>(type.num_output_ports) == descriptor.num_output_ports;
}

void passthrough_scalar(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                        const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs[0];
}

void constant_scalar(void*, const PreparedSampleKernelConfig& config, const SampleFrameContext&,
                     const float*, float* outputs) noexcept {
    outputs[0] = config.constant;
}

void parameter_scalar(void*, const PreparedSampleKernelConfig& config,
                      const SampleFrameContext& frame, const float*, float* outputs) noexcept {
    const auto index = config.boundary_or_parameter_index;
    outputs[0] = frame.promoted_values != nullptr && index < frame.promoted_value_count
                     ? frame.promoted_values[index]
                     : 0.0f;
}

void add_scalar(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs[0] + inputs[1];
}

void multiply_scalar(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                     const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs[0] * inputs[1];
}

bool delay_construct(void* state, const SampleKernelPrepareContext& context) noexcept {
    if (state == nullptr || !std::isfinite(context.sample_rate) || context.sample_rate <= 0.0 ||
        context.max_block_size == 0 ||
        context.config.kind != PreparedSampleKernelConfigKind::None) {
        return false;
    }
    std::construct_at(static_cast<float*>(state), 0.0f);
    return true;
}

void delay_reset(void* state) noexcept {
    *static_cast<float*>(state) = 0.0f;
}
void delay_destroy(void* state) noexcept {
    std::destroy_at(static_cast<float*>(state));
}
void delay_publish(const void* state, const PreparedSampleKernelConfig&, float* outputs) noexcept {
    outputs[0] = *static_cast<const float*>(state);
}
void delay_commit(void* state, const PreparedSampleKernelConfig&, const float* inputs) noexcept {
    *static_cast<float*>(state) = inputs[0];
}

struct BuiltinSampleKernelRegistration {
    CustomNodeType custom;
    SampleKernelDescriptor scalar;
};

BuiltinSampleKernelRegistration make_builtin_sample_kernel(const char* id, const char* name,
                                                           int inputs, int outputs,
                                                           SampleKernelConfigKind config_kind,
                                                           SampleKernelProcessFn process,
                                                           const char* category) {
    BuiltinSampleKernelRegistration result;
    result.custom.type_id = id;
    result.custom.version = 1;
    result.custom.num_input_ports = inputs;
    result.custom.num_output_ports = outputs;
    result.custom.default_name = name;
    result.scalar.type_id = id;
    result.scalar.version = 1;
    result.scalar.num_input_ports = static_cast<std::uint32_t>(inputs);
    result.scalar.num_output_ports = static_cast<std::uint32_t>(outputs);
    result.scalar.authored_config_kind = config_kind;
    result.scalar.process = process;
    result.scalar.metadata.category = category;
    return result;
}

std::array<BuiltinSampleKernelRegistration, 7> builtin_sample_kernels() {
    auto input = make_builtin_sample_kernel("pulp.core.sample-region.input", "Sample Region Input",
                                            1, 1, SampleKernelConfigKind::BoundaryIndex,
                                            passthrough_scalar, "boundary");
    auto output = make_builtin_sample_kernel(
        "pulp.core.sample-region.output", "Sample Region Output", 1, 1,
        SampleKernelConfigKind::BoundaryIndex, passthrough_scalar, "boundary");
    auto constant = make_builtin_sample_kernel("pulp.core.sample-region.constant", "Constant", 0, 1,
                                               SampleKernelConfigKind::FiniteConstant,
                                               constant_scalar, "source");
    constant.scalar.metadata.parameter = "value";
    constant.scalar.metadata.has_value_range = true;
    constant.scalar.metadata.minimum_value = -std::numeric_limits<float>::max();
    constant.scalar.metadata.maximum_value = std::numeric_limits<float>::max();
    auto parameter = make_builtin_sample_kernel("pulp.core.sample-region.parameter", "Parameter", 0,
                                                1, SampleKernelConfigKind::PromotedParameterId,
                                                parameter_scalar, "source");
    parameter.scalar.metadata.parameter = "promoted_parameter";
    auto add = make_builtin_sample_kernel("pulp.core.sample-region.add", "Add", 2, 1,
                                          SampleKernelConfigKind::None, add_scalar, "math");
    auto multiply =
        make_builtin_sample_kernel("pulp.core.sample-region.multiply", "Multiply", 2, 1,
                                   SampleKernelConfigKind::None, multiply_scalar, "math");
    auto delay = make_builtin_sample_kernel("pulp.core.unit-delay", "Unit Delay", 1, 1,
                                            SampleKernelConfigKind::None, nullptr, "delay");
    delay.scalar.causality = SampleKernelCausality::OneSampleDelay;
    delay.scalar.state_size = sizeof(float);
    delay.scalar.state_alignment = alignof(float);
    delay.scalar.construct = delay_construct;
    delay.scalar.reset = delay_reset;
    delay.scalar.destroy = delay_destroy;
    delay.scalar.delay_publish = delay_publish;
    delay.scalar.delay_commit = delay_commit;
    return {std::move(input), std::move(output),   std::move(constant), std::move(parameter),
            std::move(add),   std::move(multiply), std::move(delay)};
}

struct AddressRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    bool valid = true;
};

AddressRange address_range(const void* data, std::size_t bytes) noexcept {
    if (bytes == 0)
        return {};
    if (data == nullptr)
        return {0, 0, false};
    const auto begin = reinterpret_cast<std::uintptr_t>(data);
    if (bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
        return {0, 0, false};
    return {begin, begin + bytes, true};
}

bool overlaps(AddressRange lhs, AddressRange rhs) noexcept {
    return lhs.begin != lhs.end && rhs.begin != rhs.end && lhs.begin < rhs.end &&
           rhs.begin < lhs.end;
}

// Make a per-node opaque instance owned via shared_ptr, with the type's destroy
// callback as the deleter (RAII). Returns nullptr for stateless types (no
// `create`).
std::shared_ptr<void> make_custom_instance(const CustomNodeType& type) {
    if (!type.create) return nullptr;
    void* raw = type.create();
    if (raw == nullptr) return nullptr;
    auto destroy = type.destroy;
    return std::shared_ptr<void>(raw, [destroy](void* p) {
        if (destroy && p) destroy(p);
    });
}

} // namespace

bool sample_kernel_storage_is_disjoint(const SampleKernelDescriptor& descriptor, const void* state,
                                       const PreparedSampleKernelConfig& config,
                                       const SampleFrameContext& frame, const float* inputs,
                                       float* outputs) noexcept {
    const auto state_range = address_range(state, descriptor.state_size);
    const auto config_range = address_range(&config, sizeof(config));
    const auto input_range =
        address_range(inputs, static_cast<std::size_t>(descriptor.num_input_ports) * sizeof(float));
    const auto output_range = address_range(
        outputs, static_cast<std::size_t>(descriptor.num_output_ports) * sizeof(float));
    const auto promoted_range =
        address_range(frame.promoted_values,
                      static_cast<std::size_t>(frame.promoted_value_count) * sizeof(float));
    if (!state_range.valid || !config_range.valid || !input_range.valid || !output_range.valid ||
        !promoted_range.valid) {
        return false;
    }
    if ((descriptor.state_size == 0) != (state == nullptr))
        return false;
    return !overlaps(output_range, state_range) && !overlaps(output_range, config_range) &&
           !overlaps(output_range, input_range) && !overlaps(output_range, promoted_range) &&
           !overlaps(config_range, state_range) && !overlaps(config_range, input_range) &&
           !overlaps(config_range, promoted_range);
}

SignalGraph::MidiBlockSnapshot::MidiBlockSnapshot() {
    prepare_midi_block_storage(events, ump);
}

SignalGraph::MidiBlockSnapshot::MidiBlockSnapshot(
    const MidiBlockSnapshot& other)
    : MidiBlockSnapshot() {
    *this = other;
}

SignalGraph::MidiBlockSnapshot&
SignalGraph::MidiBlockSnapshot::operator=(const MidiBlockSnapshot& other) noexcept {
    if (this == &other) return *this;
    set_from_midi(other.events, other.sequence, other.incomplete);
    return *this;
}

bool SignalGraph::MidiBlockSnapshot::set_from_midi(
    const midi::MidiBuffer& src,
    uint64_t new_sequence,
    bool source_incomplete) noexcept {
    clear_midi_block(events);
    sequence = new_sequence;
    const bool copied_all = copy_midi_block(src, events);
    incomplete = source_incomplete || !copied_all;
    return !incomplete;
}

bool SignalGraph::MidiBlockSnapshot::copy_to_midi(
    midi::MidiBuffer& dst) const noexcept {
    const bool copied_all = copy_midi_block(events, dst);
    return copied_all && !incomplete;
}

bool SignalGraph::MidiBlockSnapshot::append_to_midi(
    midi::MidiBuffer& dst,
    std::size_t& event_index,
    std::size_t& sysex_index,
    std::size_t& ump_index) const noexcept {
    while (event_index < events.size()) {
        if (!dst.add(events[event_index])) return false;
        ++event_index;
    }
    const auto& source_sysex = events.sysex();
    while (sysex_index < source_sysex.size()) {
        const auto& source = source_sysex[sysex_index];
        const bool added = source.data.empty()
            ? dst.add_sysex({}, source.sample_offset, source.timestamp)
            : dst.add_sysex_copy(source.data.data(), source.data.size(),
                                 source.sample_offset, source.timestamp);
        if (!added) return false;
        ++sysex_index;
    }
    auto* destination_ump = dst.ump();
    while (ump_index < ump.size()) {
        if (destination_ump == nullptr
            || !destination_ump->add(ump[ump_index])) {
            return false;
        }
        ++ump_index;
    }
    return true;
}

bool SignalGraph::MidiBlockSnapshot::has_payload() const noexcept {
    return !events.empty() || events.sysex_size() != 0 || !ump.empty()
        || incomplete;
}

bool SignalGraph::ParameterBlockSnapshot::set_from_queue(
    const state::ParameterEventQueue& src,
    std::uint64_t new_sequence) noexcept {
    size = 0;
    sequence = new_sequence;
    source_incomplete = src.overflowed();
    for (const auto& event : src) {
        if (size == events.size()) {
            source_incomplete = true;
            break;
        }
        events[size++] = event;
    }
    return !source_incomplete;
}

bool SignalGraph::ParameterBlockSnapshot::append_to(
    state::ParameterEventQueue& dst) const noexcept {
    bool copied_all = !source_incomplete;
    for (std::size_t i = 0; i < size; ++i) {
        copied_all = dst.push(events[i]) && copied_all;
    }
    return copied_all;
}

// All add_*/connect/remove mutators below take graph_mutation_mutex_ for the
// duration of the nodes_/connections_ mutation + invalidate_live_locked_(). The lock is
// safe to hold across invalidate_live_locked_() because that drives only the
// non-blocking Slot::reclaim_if_quiescent() (never the blocking reader-drain), so
// it cannot invert lock order with the Slot reader-pin. See the
// graph_mutation_mutex_ contract in signal_graph.hpp.
NodeId SignalGraph::add_input_node(int channels, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioInput;
    node.name = name;
    node.num_output_ports = channels;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_output_node(int channels, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioOutput;
    node.name = name;
    node.num_input_ports = channels;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(const PluginInfo& info) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = info.name;
    node.num_input_ports = info.num_inputs;
    node.num_output_ports = info.num_outputs;
    node.plugin = std::shared_ptr<PluginSlot>(PluginSlot::load(info));
    node.plugin_info = info;  // preserve identity even if load failed
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_unresolved_plugin_node(const PluginInfo& info,
                                               int num_inputs,
                                               int num_outputs,
                                               const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.plugin_info = info;
    node.plugin_info.num_inputs = num_inputs;
    node.plugin_info.num_outputs = num_outputs;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(std::unique_ptr<PluginSlot> slot,
                                    int num_inputs, int num_outputs,
                                    const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.plugin = std::shared_ptr<PluginSlot>(std::move(slot));
    if (node.plugin) {
        node.plugin_info = node.plugin->info();
    } else {
        node.plugin_info.name = name;
        node.plugin_info.num_inputs = num_inputs;
        node.plugin_info.num_outputs = num_outputs;
    }
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_processor_node(std::shared_ptr<format::ProcessorNodeInstance> instance,
                                       const std::string& name) {
    if (!instance)
        return 0;
    const auto& descriptor = instance->descriptor();
    const int inputs = descriptor.default_input_channels();
    const int outputs = descriptor.default_output_channels();
    if (inputs < 0 || outputs < 0)
        return 0;

    GraphMutationLock mutation_lock(*this);
    if (std::any_of(processor_nodes_.begin(), processor_nodes_.end(), [&](const auto& entry) {
            return entry.second && entry.second->instance == instance;
        })) {
        return 0;
    }
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = name.empty() ? descriptor.name : name;
    node.num_input_ports = inputs;
    node.num_output_ports = outputs;
    node.plugin_info.name = node.name;
    node.plugin_info.manufacturer = descriptor.manufacturer;
    node.plugin_info.version = descriptor.version;
    node.plugin_info.num_inputs = inputs;
    node.plugin_info.num_outputs = outputs;
    const NodeId id = node.id;
    try {
        processor_nodes_.emplace(id, std::make_shared<ProcessorNodeLifetime>(std::move(instance)));
        nodes_.push_back(std::move(node));
    } catch (...) {
        processor_nodes_.erase(id);
        return 0;
    }
    invalidate_live_locked_();
    return id;
}

NodeId SignalGraph::add_processor_node(std::unique_ptr<format::Processor> processor,
                                       const std::string& name) {
    return add_processor_node(format::ProcessorNodeInstance::create(std::move(processor)), name);
}

bool SignalGraph::is_processor_node(NodeId id) const {
    GraphMutationLock mutation_lock(*this);
    return processor_nodes_.contains(id);
}

NodeId SignalGraph::add_gain_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Gain;
    node.name = name;
    node.num_input_ports = 2;
    node.num_output_ports = 2;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_input_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiInput;
    node.name = name;
    node.num_output_ports = 1;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_output_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiOutput;
    node.name = name;
    node.num_input_ports = 1;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

bool SignalGraph::register_custom_node_type(CustomNodeType type) {
    if (!type.is_valid_registration()) return false;
    // Scans nodes_ and mutates custom_node_types_; serialize against a concurrent
    // prepare()/mutator. custom_node_type() reads custom_node_types_ lock-free and
    // assumes the caller holds this mutex (true for every internal caller below).
    GraphMutationLock mutation_lock(*this);
    for (const auto& definition : sample_region_definitions_) {
        for (const auto& member : definition.members) {
            if (member.type_id == type.type_id && member.version == type.version)
                return false;
        }
    }
    cancel_swap_edit_locked_();
    const bool affects_existing_nodes = std::any_of(
        nodes_.begin(), nodes_.end(), [&](const GraphNode& node) {
            return node.type == NodeType::Custom
                && node.custom_type_id == type.type_id
                && node.custom_type_version == type.version;
        });
    if (type.default_name.empty()) type.default_name = type.type_id;
    const auto key = custom_node_key(type.type_id, type.version);
    // A block-only replacement deliberately withdraws any scalar companion.
    // This preserves the historical one-argument replacement behavior without
    // leaving a stale descriptor paired with different callbacks.
    sample_kernel_types_.erase(key);
    custom_node_diagnostics_.erase(key);
    custom_node_types_[key] = std::move(type);
    // M6 (2.2b): any registry change bumps the generation so a reinit-free swap
    // compiled against an older generation is rejected (a re-register rebinds
    // callbacks to instances the old factory produced).
    ++custom_registry_generation_;
    if (affects_existing_nodes) {
        invalidate_live_locked_();
    } else {
        ++authoring_generation_;
    }
    return true;
}

bool SignalGraph::register_custom_node_type(CustomNodeType type,
                                            SampleKernelDescriptor sample_kernel) {
    // A scalar-paired type must not declare the event lane. A region MEMBER is
    // quotiented out of the executable topology and an ANCHOR has its event
    // binding dropped at compile, so either way the callback would be registered
    // and then never invoked — the same silent MIDI-less degradation that
    // `lowerable && consumes_events()` is refused for. Refuse it here too, where
    // it is still visible to the registrant.
    if (!type.is_valid_registration() || !sample_kernel.is_valid_registration() ||
        type.consumes_events() || !scalar_identity_matches(type, sample_kernel)) {
        return false;
    }
    if (type.default_name.empty())
        type.default_name = type.type_id;

    GraphMutationLock mutation_lock(*this);
    const auto key = custom_node_key(type.type_id, type.version);
    const auto existing_scalar = sample_kernel_types_.find(key);
    const auto existing_custom = custom_node_types_.find(key);
    if (existing_scalar != sample_kernel_types_.end()) {
        return existing_custom != custom_node_types_.end() &&
               sample_descriptors_equal(existing_scalar->second, sample_kernel) &&
               custom_types_equal_for_idempotency(existing_custom->second, type);
    }
    if (existing_custom != custom_node_types_.end() &&
        !custom_types_equal_for_idempotency(existing_custom->second, type)) {
        return false;
    }

    const bool affects_existing_nodes =
        std::any_of(nodes_.begin(), nodes_.end(), [&](const GraphNode& node) {
            return node.type == NodeType::Custom && node.custom_type_id == type.type_id &&
                   node.custom_type_version == type.version;
        });
    // Build the full replacement off-side so allocation failure cannot publish
    // only one half of the pair.
    auto next_custom = custom_node_types_;
    auto next_scalar = sample_kernel_types_;
    if (existing_custom == custom_node_types_.end()) {
        next_custom[key] = std::move(type);
    }
    next_scalar[key] = std::move(sample_kernel);
    cancel_swap_edit_locked_();
    custom_node_types_.swap(next_custom);
    sample_kernel_types_.swap(next_scalar);
    ++custom_registry_generation_;
    if (affects_existing_nodes)
        invalidate_live_locked_();
    else
        ++authoring_generation_;
    return true;
}

std::vector<CustomNodeTypeMetadata> SignalGraph::custom_node_types() const {
    GraphMutationLock mutation_lock(*this);
    std::vector<CustomNodeTypeMetadata> snapshot;
    snapshot.reserve(custom_node_types_.size());
    for (const auto& [_, type] : custom_node_types_) {
        snapshot.push_back(CustomNodeTypeMetadata{
            type.type_id,
            type.version,
            type.num_input_ports,
            type.num_output_ports,
            type.default_name,
            type.lowerable,
            type.baked_params,
            type.consumes_events(),
        });
    }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const CustomNodeTypeMetadata& lhs, const CustomNodeTypeMetadata& rhs) {
                  if (lhs.type_id != rhs.type_id)
                      return lhs.type_id < rhs.type_id;
                  return lhs.version < rhs.version;
              });
    return snapshot;
}

std::size_t SignalGraph::custom_node_type_count() const {
    GraphMutationLock mutation_lock(*this);
    return custom_node_types_.size();
}

const CustomNodeType* SignalGraph::custom_node_type(std::string_view type_id) const {
    const CustomNodeType* latest = nullptr;
    const std::string wanted(type_id);
    for (const auto& [_, type] : custom_node_types_) {
        if (type.type_id != wanted) continue;
        if (!latest || type.version > latest->version) latest = &type;
    }
    return latest;
}

const CustomNodeType* SignalGraph::custom_node_type(std::string_view type_id,
                                                    int version) const {
    auto it = custom_node_types_.find(custom_node_key(type_id, version));
    if (it == custom_node_types_.end()) return nullptr;
    return &it->second;
}

const SampleKernelDescriptor* SignalGraph::sample_kernel_type(std::string_view type_id,
                                                              int version) const {
    const auto found = sample_kernel_types_.find(custom_node_key(type_id, version));
    return found == sample_kernel_types_.end() ? nullptr : &found->second;
}

namespace {

SampleRegionProof authoring_refusal(SampleRegionId id, SampleRegionRefusalReason reason,
                                    std::string message, NodeId node = 0) {
    SampleRegionProof result;
    result.region_id = id;
    result.reason = reason;
    result.offending_node = node;
    result.message = std::move(message);
    return result;
}

SampleRegionConnection region_connection(const Connection& connection) {
    SampleRegionConnection result{connection.source_node, connection.source_port,
                                  connection.dest_node, connection.dest_port};
    result.legacy_feedback = connection.feedback;
    if (connection.midi)
        result.lane = SampleRegionConnectionLane::Midi;
    else if (connection.automation)
        result.lane = SampleRegionConnectionLane::Automation;
    else if (connection.audio_rate_modulation)
        result.lane = SampleRegionConnectionLane::AudioRateModulation;
    else if (connection.sidechain)
        result.lane = SampleRegionConnectionLane::Sidechain;
    return result;
}

bool valid_authored_config(const SampleKernelConfig& config) {
    switch (config.kind) {
    case SampleKernelConfigKind::None:
        return config.boundary_index_or_parameter_id == 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::BoundaryIndex:
        return config.constant == 0.0f;
    case SampleKernelConfigKind::FiniteConstant:
        return config.boundary_index_or_parameter_id == 0 && std::isfinite(config.constant);
    case SampleKernelConfigKind::PromotedParameterId:
        return config.boundary_index_or_parameter_id != 0 && config.constant == 0.0f;
    case SampleKernelConfigKind::Invalid:
        return false;
    }
    return false;
}

} // namespace

SampleRegionId SignalGraph::sample_region_for_node_locked_(NodeId id) const {
    assert_graph_mutation_locked_();
    for (const auto& definition : sample_region_definitions_) {
        for (const auto& member : definition.members) {
            if (member.node == id)
                return definition.region_id;
        }
    }
    return 0;
}

bool SignalGraph::has_sample_kernel_nodes_locked_() const {
    assert_graph_mutation_locked_();
    return std::any_of(nodes_.begin(), nodes_.end(), [&](const GraphNode& node) {
        return node.type == NodeType::Custom &&
               sample_kernel_type(node.custom_type_id, node.custom_type_version) != nullptr;
    });
}

SampleRegionProof
SignalGraph::sample_region_metadata_proof_locked_(const SampleRegionDefinition& definition,
                                                  bool complete) const {
    assert_graph_mutation_locked_();
    const auto fail = [&](SampleRegionRefusalReason reason, std::string message, NodeId node = 0) {
        return authoring_refusal(definition.region_id, reason, std::move(message), node);
    };
    if (definition.region_id == 0)
        return fail(SampleRegionRefusalReason::UnknownRegion, "region ID must be nonzero");
    std::unordered_map<NodeId, const SampleRegionKernelNode*> members;
    for (const auto& member : definition.members) {
        if (!members.emplace(member.node, &member).second)
            return fail(SampleRegionRefusalReason::UnknownMember, "duplicate member ID",
                        member.node);
        const auto* current = node(member.node);
        if (current == nullptr)
            return fail(SampleRegionRefusalReason::UnknownMember, "member node does not exist",
                        member.node);
        if (current->type != NodeType::Custom)
            return fail(SampleRegionRefusalReason::UnsupportedNodeKind,
                        "region members must be exact custom kernel nodes", member.node);
        if (current->custom_type_id != member.type_id ||
            current->custom_type_version != member.version)
            return fail(SampleRegionRefusalReason::UnresolvedSampleKernel,
                        "member identity differs from its candidate node", member.node);
        const auto owner = sample_region_for_node_locked_(member.node);
        if (owner != 0 && owner != definition.region_id)
            return fail(SampleRegionRefusalReason::MemberInMultipleRegions,
                        "member already belongs to another region", member.node);
        const auto* kernel = sample_kernel_type(member.type_id, member.version);
        if (kernel == nullptr)
            return fail(SampleRegionRefusalReason::UnresolvedSampleKernel,
                        "member requires an exact registered scalar descriptor", member.node);
        if (kernel != nullptr &&
            (kernel->num_input_ports != static_cast<std::uint32_t>(current->num_input_ports) ||
             kernel->num_output_ports != static_cast<std::uint32_t>(current->num_output_ports)))
            return fail(SampleRegionRefusalReason::UnresolvedSampleKernel,
                        "member ports differ from its exact kernel descriptor", member.node);
        if (!valid_authored_config(member.config) ||
            (kernel != nullptr && member.config.kind != kernel->authored_config_kind))
            return fail(SampleRegionRefusalReason::InvalidKernelConfig,
                        "authored config does not match the exact kernel contract", member.node);
    }
    for (const bool input : {true, false}) {
        const auto& boundaries = input ? definition.input_boundaries : definition.output_boundaries;
        const std::string_view type =
            input ? "pulp.core.sample-region.input" : "pulp.core.sample-region.output";
        std::unordered_set<NodeId> unique;
        for (const auto id : boundaries) {
            const auto found = members.find(id);
            if (!unique.insert(id).second || found == members.end() ||
                found->second->type_id != type ||
                found->second->config.kind != SampleKernelConfigKind::BoundaryIndex)
                return fail(SampleRegionRefusalReason::InvalidBoundary,
                            "boundary identity must name a unique matching member", id);
        }
        if (complete) {
            for (const auto& member : definition.members) {
                if (member.type_id == type && !unique.contains(member.node))
                    return fail(SampleRegionRefusalReason::InvalidBoundary,
                                "boundary member is absent from explicit boundary metadata",
                                member.node);
            }
        }
    }
    std::unordered_set<state::ParamID> ids;
    std::unordered_set<std::string> keys;
    for (const auto& parameter : definition.promoted_parameters) {
        if (parameter.param_id == 0 || !ids.insert(parameter.param_id).second)
            return fail(SampleRegionRefusalReason::DuplicatePromotedParameter,
                        "promoted parameter IDs must be unique and nonzero",
                        parameter.bound_node_id);
        if (!keys.insert(parameter.key).second)
            return fail(SampleRegionRefusalReason::ParameterContractMismatch,
                        "promoted parameter keys must be unique in each region",
                        parameter.bound_node_id);
        for (const auto& other : sample_region_definitions_) {
            if (other.region_id == definition.region_id)
                continue;
            for (const auto& existing : other.promoted_parameters) {
                if (existing.param_id == parameter.param_id)
                    return fail(SampleRegionRefusalReason::DuplicatePromotedParameter,
                                "promoted parameter ID is already used by another region",
                                parameter.bound_node_id);
            }
        }
        if (!complete)
            continue;
        const auto& range = parameter.range;
        const auto member = members.find(parameter.bound_node_id);
        if (parameter.key.empty() || parameter.name.empty() || !std::isfinite(range.min) ||
            !std::isfinite(range.max) || !std::isfinite(range.default_value) ||
            !std::isfinite(range.step) || !std::isfinite(range.skew) || range.min > range.max ||
            range.default_value < range.min || range.default_value > range.max ||
            range.step < 0.0f || range.skew <= 0.0f ||
            parameter.rate != state::ParamRate::ControlRate ||
            parameter.smoothing_ramp_seconds != 0.0f || parameter.bound_port != 0 ||
            member == members.end() ||
            member->second->type_id != "pulp.core.sample-region.parameter" ||
            member->second->config.kind != SampleKernelConfigKind::PromotedParameterId ||
            member->second->config.boundary_index_or_parameter_id != parameter.param_id)
            return fail(SampleRegionRefusalReason::ParameterContractMismatch,
                        "promoted metadata and parameter-source binding must match exactly",
                        parameter.bound_node_id);
    }
    SampleRegionProof result;
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    result.region_id = definition.region_id;
    return result;
}

SampleRegionCandidate
SignalGraph::sample_region_candidate_locked_(const SampleRegionDefinition& definition) const {
    assert_graph_mutation_locked_();
    SampleRegionCandidate candidate;
    candidate.region_id = definition.region_id;
    candidate.registry = {
        this, [](const void* context, std::string_view type_id, int version) noexcept {
            return static_cast<const SignalGraph*>(context)->sample_kernel_type(type_id, version);
        }};
    candidate.members = definition.members;
    candidate.limits = definition.limits;
    candidate.max_block_size = sample_region_proof_block_size_;
    for (const auto& parameter : definition.promoted_parameters)
        candidate.promoted_parameters.push_back(parameter.param_id);
    for (const auto& connection : connections_) {
        const auto touches = [&](NodeId id) {
            return std::any_of(definition.members.begin(), definition.members.end(),
                               [&](const auto& member) { return member.node == id; });
        };
        if (touches(connection.source_node) || touches(connection.dest_node))
            candidate.connections.push_back(region_connection(connection));
    }
    return candidate;
}

SampleRegionProof SignalGraph::sample_region_exterior_proof_locked_() const {
    assert_graph_mutation_locked_();
    std::unordered_map<NodeId, NodeId> representative;
    std::unordered_map<NodeId, SampleRegionId> region_ids;
    for (const auto& node : nodes_)
        representative.emplace(node.id, node.id);
    for (const auto& definition : sample_region_definitions_) {
        if (definition.members.empty())
            continue;
        const auto first = definition.members.front().node;
        region_ids[first] = definition.region_id;
        for (const auto& member : definition.members)
            representative[member.node] = first;
    }
    std::unordered_map<NodeId, std::size_t> indegree;
    std::unordered_map<NodeId, std::vector<NodeId>> outgoing;
    for (const auto& [id, rep] : representative)
        indegree.try_emplace(rep, 0);
    for (const auto& connection : connections_) {
        if (connection.feedback)
            continue;
        const auto source = representative.at(connection.source_node);
        const auto destination = representative.at(connection.dest_node);
        if (source == destination && region_ids.contains(source))
            continue;
        outgoing[source].push_back(destination);
        ++indegree[destination];
    }
    std::queue<NodeId> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0)
            ready.push(id);
    }
    while (!ready.empty()) {
        const auto id = ready.front();
        ready.pop();
        for (const auto next : outgoing[id]) {
            if (--indegree[next] == 0)
                ready.push(next);
        }
    }
    for (const auto& definition : sample_region_definitions_) {
        if (!definition.members.empty() && indegree.at(definition.members.front().node) != 0)
            return authoring_refusal(definition.region_id,
                                     SampleRegionRefusalReason::CycleCrossesRegionBoundary,
                                     "the exterior graph must remain acyclic around each region");
    }
    for (const auto& current : nodes_) {
        if (indegree.at(representative.at(current.id)) != 0)
            return authoring_refusal(0, SampleRegionRefusalReason::InstantaneousCycle,
                                     "the exterior graph contains an ordinary cycle", current.id);
    }
    SampleRegionProof result;
    result.accepted = true;
    result.reason = SampleRegionRefusalReason::None;
    return result;
}

SampleRegionProof SignalGraph::sample_region_proof_locked_(SampleRegionId id) const {
    assert_graph_mutation_locked_();
    const auto found =
        std::find_if(sample_region_definitions_.begin(), sample_region_definitions_.end(),
                     [&](const auto& definition) { return definition.region_id == id; });
    if (found == sample_region_definitions_.end())
        return authoring_refusal(id, SampleRegionRefusalReason::UnknownRegion,
                                 "region does not exist");
    auto proof = sample_region_metadata_proof_locked_(*found);
    if (!proof.accepted)
        return proof;
    std::vector<SampleRegionCandidate> candidates;
    for (const auto& definition : sample_region_definitions_) {
        if (definition.region_id != id) {
            const auto metadata = sample_region_metadata_proof_locked_(definition);
            if (!metadata.accepted)
                return authoring_refusal(id, metadata.reason,
                                         "another candidate region has invalid metadata: " +
                                             metadata.message,
                                         metadata.offending_node);
        }
        candidates.push_back(sample_region_candidate_locked_(definition));
    }
    const auto graph_proof = pulp::host::prove_sample_regions(candidates);
    if (!graph_proof.accepted) {
        if (graph_proof.region_proof.region_id == id)
            return graph_proof.region_proof;
        auto refusal = authoring_refusal(id, graph_proof.reason,
                                         "the complete candidate graph failed sample-region proof");
        refusal.actual = graph_proof.actual;
        refusal.limit = graph_proof.limit;
        return refusal;
    }
    for (const auto& current : nodes_) {
        if (current.type == NodeType::Custom &&
            sample_kernel_type(current.custom_type_id, current.custom_type_version) != nullptr &&
            sample_region_for_node_locked_(current.id) == 0)
            return authoring_refusal(id, SampleRegionRefusalReason::SampleKernelOutsideRegion,
                                     "a scalar kernel is outside every declared region",
                                     current.id);
    }
    proof = pulp::host::prove_sample_region(sample_region_candidate_locked_(*found));
    auto exterior = sample_region_exterior_proof_locked_();
    if (!exterior.accepted && exterior.region_id == 0)
        exterior.region_id = id;
    return exterior.accepted ? proof : exterior;
}

SampleRegionProof SignalGraph::prove_sample_region(SampleRegionId id) const {
    GraphMutationLock mutation_lock(*this);
    return sample_region_proof_locked_(id);
}

std::optional<SampleRegionDescriptor> SignalGraph::sample_region(SampleRegionId id) const {
    GraphMutationLock mutation_lock(*this);
    for (const auto& definition : sample_region_definitions_) {
        if (definition.region_id != id)
            continue;
        SampleRegionDescriptor descriptor;
        static_cast<SampleRegionDefinition&>(descriptor) = definition;
        descriptor.resources = sample_region_proof_locked_(id).resources;
        return descriptor;
    }
    return std::nullopt;
}

std::vector<SampleRegionDescriptor> SignalGraph::sample_regions() const {
    GraphMutationLock mutation_lock(*this);
    std::vector<SampleRegionDescriptor> result;
    for (const auto& definition : sample_region_definitions_) {
        SampleRegionDescriptor descriptor;
        static_cast<SampleRegionDefinition&>(descriptor) = definition;
        descriptor.resources = sample_region_proof_locked_(definition.region_id).resources;
        result.push_back(std::move(descriptor));
    }
    return result;
}

bool register_builtin_sample_region_types(SignalGraph& graph) {
    auto cohort = builtin_sample_kernels();
    for (const auto& registration : cohort) {
        if (!registration.custom.is_valid_registration() ||
            !registration.scalar.is_valid_registration() ||
            !scalar_identity_matches(registration.custom, registration.scalar)) {
            return false;
        }
    }

    SignalGraph::GraphMutationLock mutation_lock(graph);
    auto next_custom = graph.custom_node_types_;
    auto next_scalar = graph.sample_kernel_types_;
    bool changed = false;
    bool affects_existing_nodes = false;
    for (auto& registration : cohort) {
        const auto key = custom_node_key(registration.custom.type_id, registration.custom.version);
        const auto existing_custom = next_custom.find(key);
        const auto existing_scalar = next_scalar.find(key);
        if (existing_scalar != next_scalar.end()) {
            if (existing_custom == next_custom.end() ||
                !custom_types_equal_for_idempotency(existing_custom->second, registration.custom) ||
                !sample_descriptors_equal(existing_scalar->second, registration.scalar)) {
                return false;
            }
            continue;
        }
        if (existing_custom != next_custom.end() &&
            !custom_types_equal_for_idempotency(existing_custom->second, registration.custom)) {
            return false;
        }
        affects_existing_nodes =
            affects_existing_nodes ||
            std::any_of(graph.nodes_.begin(), graph.nodes_.end(), [&](const GraphNode& node) {
                return node.type == NodeType::Custom &&
                       node.custom_type_id == registration.custom.type_id &&
                       node.custom_type_version == registration.custom.version;
            });
        next_custom[key] = std::move(registration.custom);
        next_scalar[key] = std::move(registration.scalar);
        changed = true;
    }
    if (!changed)
        return true;
    graph.cancel_swap_edit_locked_();
    graph.custom_node_types_.swap(next_custom);
    graph.sample_kernel_types_.swap(next_scalar);
    ++graph.custom_registry_generation_;
    if (affects_existing_nodes)
        graph.invalidate_live_locked_();
    else
        ++graph.authoring_generation_;
    return true;
}

// add_custom_node overloads only RESOLVE a registered type (read
// custom_node_types_) and delegate to the public add_unresolved_custom_node leaf,
// which takes graph_mutation_mutex_ once for the nodes_ mutation. They do NOT
// take the lock themselves — delegating through another locked public method
// would self-deadlock this non-recursive mutex. Type registration is expected to
// precede topology building (register_custom_node_type holds the same mutex), so
// the custom_node_types_ read here is consistent with that ordering.
NodeId SignalGraph::add_custom_node(std::string_view type_id,
                                    const std::string& name) {
    const auto* type = custom_node_type(type_id);
    if (!type) return 0;
    return add_custom_node(type_id, type->version, name);
}

NodeId SignalGraph::add_custom_node(std::string_view type_id,
                                    int version,
                                    const std::string& name) {
    const auto* type = custom_node_type(type_id, version);
    if (!type) return 0;
    return add_unresolved_custom_node(
        type->type_id,
        type->version,
        type->num_input_ports,
        type->num_output_ports,
        name.empty() ? type->default_name : name);
}

NodeId SignalGraph::add_unresolved_custom_node(std::string_view type_id,
                                               int version,
                                               int num_inputs,
                                               int num_outputs,
                                               const std::string& name) {
    CustomNodeType type;
    type.type_id = std::string(type_id);
    type.version = version;
    type.num_input_ports = num_inputs;
    type.num_output_ports = num_outputs;
    type.default_name = name;
    if (!type.is_valid_registration()) return 0;

    // The single locked leaf for the add_custom_node family — serializes the
    // nodes_ push against a concurrent prepare()/mutator.
    GraphMutationLock mutation_lock(*this);
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Custom;
    node.name = name.empty() ? type.type_id : name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.custom_type_id = std::move(type.type_id);
    node.custom_type_version = version;
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

bool SignalGraph::remove_node(NodeId id) {
    // Serialize the nodes_ erase against a concurrent compile_()/node() scan on a
    // host thread (see add_gain_node for the lock-ordering rationale).
    GraphMutationLock mutation_lock(*this);
    if (sample_region_for_node_locked_(id) != 0)
        return false;
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [id](const GraphNode& n) { return n.id == id; });
    if (it == nodes_.end()) return false;
    for (std::size_t i = connections_.size(); i-- > 0;) {
        const auto& c = connections_[i];
        if (c.source_node == id || c.dest_node == id) {
            erase_connection_at_locked_(i);
        }
    }
    nodes_.erase(it);
    processor_nodes_.erase(id);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect(NodeId source, PortIndex source_port,
                          NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_port)) return false;
    if (would_create_cycle(source, dest)) return false;
    Connection conn{source, source_port, dest, dest_port};
    for (auto& c : connections_) if (c == conn) return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_midi(NodeId source, NodeId dest) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    if (!node(source) || !node(dest)) return false;
    if (would_create_cycle(source, dest)) return false;
    Connection conn{source, 0, dest, 0, false, true};
    for (auto& c : connections_) if (c == conn && c.midi == conn.midi) return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_sidechain(NodeId source, PortIndex source_port,
                                    NodeId dest, PortIndex dest_sidechain_port) {
    // Sidechain connections only make sense to Plugin nodes; everything
    // else (Gain, Custom, AudioOutput, ...) has no notion of a sidechain
    // bus. We reject other destinations early so callers fail loudly
    // instead of silently routing into a regular audio port.
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_sidechain_port)) return false;
    if (would_create_cycle(source, dest)) return false;

    Connection conn{};
    conn.source_node = source;
    conn.source_port = source_port;
    conn.dest_node = dest;
    conn.dest_port = dest_sidechain_port;
    conn.sidechain = true;
    for (auto& c : connections_) if (c == conn) return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_automation(NodeId src, PortIndex src_audio_port,
                                     NodeId dest, uint32_t dest_param_id,
                                     float range_lo, float range_hi,
                                     float smoothing_ms,
                                     AutomationMix mix) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(src) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin || (!dst_n->plugin && !processor_nodes_.contains(dest)))
        return false;
    if (!has_output_port(*src_n, src_audio_port)) return false;

    // Reject automation edges that would introduce a cycle. Automation
    // edges contribute to topological order (the source must be processed
    // before the dest), so a back-edge here would make the graph
    // un-orderable. Use the same has_path_locked_ check as connect().
    if (would_create_cycle(src, dest)) return false;

    // Parameter must exist, be automatable, and not read-only.
    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id) continue;
        if (!pi.flags.automatable || pi.flags.read_only) return false;
        ok_param = true;
        break;
    }
    if (!ok_param) return false;

    // Second Replace edge to same (dest, param) is rejected.
    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.automation && c.dest_node == dest
                && c.automation_param_id == dest_param_id
                && c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node              = src;
    conn.source_port              = src_audio_port;
    conn.dest_node                = dest;
    conn.dest_port                = 0;
    conn.automation               = true;
    conn.automation_param_id      = dest_param_id;
    conn.automation_range_lo      = range_lo;
    conn.automation_range_hi      = range_hi;
    conn.automation_smoothing_ms  = std::max(0.0f, smoothing_ms);
    conn.automation_mix           = mix;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_audio_rate_modulation(NodeId src, PortIndex src_audio_port,
                                                NodeId dest, uint32_t dest_param_id,
                                                float range_lo, float range_hi,
                                                float smoothing_ms,
                                                AutomationMix mix) {
    // Holds graph_mutation_mutex_ across audio_rate_modulation_lane() below, which
    // is a lock-free helper that assumes the caller holds it (it scans nodes_).
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(src) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (dst_n->type != NodeType::Plugin || (!dst_n->plugin && !processor_nodes_.contains(dest)))
        return false;
    if (!has_output_port(*src_n, src_audio_port)) return false;
    if (would_create_cycle(src, dest)) return false;

    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id) continue;
        if (!parameter_allows_modulation(
                pi, dest_param_id, state::ParamRate::AudioRate, true)) {
            return false;
        }
        ok_param = true;
        break;
    }
    if (!ok_param) return false;

    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.audio_rate_modulation && c.dest_node == dest
                && c.automation_param_id == dest_param_id
                && c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node              = src;
    conn.source_port              = src_audio_port;
    conn.dest_node                = dest;
    conn.dest_port                = 0;
    conn.audio_rate_modulation    = true;
    conn.automation_param_id      = dest_param_id;
    conn.automation_range_lo      = range_lo;
    conn.automation_range_hi      = range_hi;
    conn.automation_smoothing_ms  = std::max(0.0f, smoothing_ms);
    conn.automation_mix           = mix;
    state::ModulationLane lane;
    // Mutex already held: call the lock-free core directly (the public
    // audio_rate_modulation_lane would re-lock and self-deadlock).
    if (!audio_rate_modulation_lane_locked_(conn, lane)) return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::audio_rate_modulation_lane(const Connection& connection,
                                             state::ModulationLane& lane) const {
    // Public entry: scans nodes_ via node(), so serialize against mutators.
    GraphMutationLock mutation_lock(*this);
    return audio_rate_modulation_lane_locked_(connection, lane);
}

bool SignalGraph::audio_rate_modulation_lane_locked_(const Connection& connection,
                                                     state::ModulationLane& lane) const {
    assert_graph_mutation_locked_();
    if (!connection.audio_rate_modulation || connection.automation) {
        return false;
    }

    const GraphNode* src_n = node(connection.source_node);
    const GraphNode* dst_n = node(connection.dest_node);
    if (!src_n || !dst_n || dst_n->type != NodeType::Plugin ||
        (!dst_n->plugin && !processor_nodes_.contains(connection.dest_node))) {
        return false;
    }
    if (!has_output_port(*src_n, connection.source_port)) {
        return false;
    }

    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != connection.automation_param_id) continue;

        lane = state::ModulationLane{
            .source = {
                .id = static_cast<state::ModulationSourceId>(connection.source_node),
                .scope = state::ModulationScope::GraphNode,
                .rate = state::ModulationRate::Audio,
            },
            .target = {
                .param_id = pi.id,
                .scope = state::ModulationScope::GraphNode,
                .param_rate = pi.rate,
                .modulatable = pi.flags.modulatable
                    && pi.flags.automatable
                    && !pi.flags.stepped,
                .writable = !pi.flags.read_only,
            },
            .mix = modulation_mix_for(connection.automation_mix),
            .depth = std::abs(connection.automation_range_hi
                              - connection.automation_range_lo),
        };
        return state::validate_modulation_lane(lane).accepted;
    }

    return false;
}

bool SignalGraph::inject_midi(NodeId id,
                              const midi::MidiBuffer& events) noexcept {
    // Pin the live snapshot for the whole dereference. Without the guard a
    // concurrent prepare()/release()/invalidate could retire+free `cg` mid-use.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return false;
    return inject_midi_into_snapshot_(*cg, id, events);
}

bool SignalGraph::inject_midi_into_snapshot_(CompiledGraph& snapshot, NodeId id,
                                              const midi::MidiBuffer& events) noexcept {
    auto it = snapshot.runtime.find(id);
    if (it == snapshot.runtime.end()) return false;
    auto shape_it = snapshot.shapes.find(id);
    if (shape_it == snapshot.shapes.end()
        || shape_it->second.type != NodeType::MidiInput
        || !it->second.midi_input_mailbox) {
        return false;
    }

    auto& mailbox = *it->second.midi_input_mailbox;
    const uint64_t sequence = next_nonzero_mailbox_sequence(mailbox.next_sequence);
    const bool copied_all = mailbox.writer_scratch.set_from_midi(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

bool SignalGraph::inject_parameter_events(
    NodeId id,
    const state::ParameterEventQueue& events) {
    // Same reader-pinned, single-writer publication discipline as inject_midi.
    // The fixed-capacity writer scratch and TripleBuffer were prepared with the
    // snapshot, so this path performs no allocation.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return false;
    // A node the timeline binding owns for exact ingress rejects ad-hoc live
    // injection, so a UI/host param write cannot race the timeline's stream.
    const auto runtime_it = cg->runtime.find(id);
    if (runtime_it != cg->runtime.end()
        && !runtime_it->second.exact_parameter_event_owner.expired()) {
        return false;
    }
    return inject_parameter_events_into_snapshot_(*cg, id, events);
}

bool SignalGraph::inject_parameter_events_into_snapshot_(
    CompiledGraph& snapshot, NodeId id,
    const state::ParameterEventQueue& events) noexcept {
    auto runtime_it = snapshot.runtime.find(id);
    if (runtime_it == snapshot.runtime.end()) return false;
    const auto shape_it = snapshot.shapes.find(id);
    if (shape_it == snapshot.shapes.end() || shape_it->second.type != NodeType::Plugin ||
        (snapshot.plugins.find(id) == snapshot.plugins.end() &&
         snapshot.processors.find(id) == snapshot.processors.end()) ||
        !runtime_it->second.parameter_input_mailbox) {
        return false;
    }

    auto& mailbox = *runtime_it->second.parameter_input_mailbox;
    const std::uint64_t sequence =
        next_nonzero_mailbox_sequence(mailbox.next_sequence);
    const bool copied_all =
        mailbox.writer_scratch.set_from_queue(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

bool SignalGraph::inject_exact_parameter_events_into_snapshot_(
    CompiledGraph& snapshot, NodeId id,
    const state::ParameterEventQueue& events) noexcept {
    auto runtime_it = snapshot.runtime.find(id);
    if (runtime_it == snapshot.runtime.end()) return false;
    const auto shape_it = snapshot.shapes.find(id);
    if (shape_it == snapshot.shapes.end() || shape_it->second.type != NodeType::Plugin ||
        (snapshot.plugins.find(id) == snapshot.plugins.end() &&
         snapshot.processors.find(id) == snapshot.processors.end()) ||
        !runtime_it->second.exact_parameter_input_mailbox ||
        runtime_it->second.exact_parameter_event_owner.expired()) {
        return false;
    }

    auto& mailbox = *runtime_it->second.exact_parameter_input_mailbox;
    const std::uint64_t sequence =
        next_nonzero_mailbox_sequence(mailbox.next_sequence);
    const bool copied_all =
        mailbox.writer_scratch.set_from_queue(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

bool SignalGraph::seed_midi_input_sequence_for_test(
    NodeId id, std::uint64_t predecessor) noexcept {
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return false;
    auto runtime_it = cg->runtime.find(id);
    const auto shape_it = cg->shapes.find(id);
    if (runtime_it == cg->runtime.end() || shape_it == cg->shapes.end()
        || shape_it->second.type != NodeType::MidiInput
        || !runtime_it->second.midi_input_mailbox) {
        return false;
    }
    runtime_it->second.midi_input_mailbox->next_sequence.store(
        predecessor, std::memory_order_relaxed);
    return true;
}

std::uint64_t SignalGraph::midi_input_sequence_for_test(NodeId id) const noexcept {
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return 0;
    const auto runtime_it = cg->runtime.find(id);
    if (runtime_it == cg->runtime.end()
        || !runtime_it->second.midi_input_mailbox) {
        return 0;
    }
    return runtime_it->second.midi_input_mailbox->next_sequence.load(
        std::memory_order_relaxed);
}

PluginBindingContext::PendingParameterEventSequences
SignalGraph::append_parameter_mailbox_events_(
    void* runtime,
    state::ParameterEventQueue& destination) noexcept {
    auto* rt = static_cast<NodeRuntime*>(runtime);
    PluginBindingContext::PendingParameterEventSequences pending;
    if (rt == nullptr) return pending;
    const auto append = [&destination](ParameterInputMailbox* mailbox,
                                       bool require_complete) noexcept {
        if (mailbox == nullptr) return std::uint64_t{0};
        const auto& injected = mailbox->published.read();
        const auto sequence_seen =
            mailbox->sequence_seen.load(std::memory_order_relaxed);
        if (injected.sequence == 0 || injected.sequence == sequence_seen) {
            return std::uint64_t{0};
        }
        const bool appended_all = injected.append_to(destination);
        return appended_all || !require_complete ? injected.sequence
                                                 : std::uint64_t{0};
    };
    // Exact-generation events follow live events so they win stable-sort ties.
    pending.live = append(rt->parameter_input_mailbox.get(), false);
    // require_complete=true is safe because a claimed node is the SOLE writer of
    // its exact mailbox: it is refused live injection and rejected from graph
    // automation (DeviceNodeAutomationConflict), so the per-block exact queue is
    // bounded by one delivery and never exceeds capacity. The complete-or-skip
    // append therefore cannot drop events or retry-starve — a partial append
    // would only occur if that sole-writer invariant were violated.
    pending.exact = rt->exact_parameter_event_owner.expired()
        ? 0
        : append(rt->exact_parameter_input_mailbox.get(), true);
    return pending;
}

bool SignalGraph::extract_midi(NodeId id, midi::MidiBuffer& out) const {
    // Pin the live snapshot for the whole dereference (see inject_midi). const
    // method: Slot::read() only touches the slot's mutable reader counter.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return false;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return false;
    auto shape_it = cg->shapes.find(id);
    if (shape_it == cg->shapes.end()
        || shape_it->second.type != NodeType::MidiOutput
        || !it->second.midi_output_mailbox) {
        return false;
    }

    auto& mailbox = *it->second.midi_output_mailbox;
    mailbox.consumer_incomplete =
        mailbox.incomplete.exchange(false, std::memory_order_relaxed)
        || mailbox.consumer_incomplete;
    if (mailbox.consumer_has_retry) {
        if (!mailbox.consumer_scratch.append_to_midi(
                out,
                mailbox.consumer_event_index,
                mailbox.consumer_sysex_index,
                mailbox.consumer_ump_index)) {
            return false;
        }
        mailbox.consumer_incomplete = mailbox.consumer_scratch.incomplete
            || mailbox.consumer_incomplete;
        mailbox.consumer_has_retry = false;
    }
    while (mailbox.pending.try_pop(mailbox.consumer_scratch)) {
        mailbox.consumer_event_index = 0;
        mailbox.consumer_sysex_index = 0;
        mailbox.consumer_ump_index = 0;
        mailbox.consumer_incomplete = mailbox.consumer_scratch.incomplete
            || mailbox.consumer_incomplete;
        if (!mailbox.consumer_scratch.append_to_midi(
                out,
                mailbox.consumer_event_index,
                mailbox.consumer_sysex_index,
                mailbox.consumer_ump_index)) {
            mailbox.consumer_has_retry = true;
            return false;
        }
    }
    const bool complete = !mailbox.consumer_incomplete;
    mailbox.consumer_incomplete = false;
    return complete;
}

bool SignalGraph::connect_feedback(NodeId source, PortIndex source_port,
                                   NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n) return false;
    if (!has_output_port(*src_n, source_port)) return false;
    if (!has_input_port(*dst_n, dest_port)) return false;
    Connection conn{source, source_port, dest, dest_port, true};
    for (auto& c : connections_) if (c == conn) return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::disconnect(NodeId source, PortIndex source_port,
                             NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    Connection target{source, source_port, dest, dest_port};
    auto it = std::find(connections_.begin(), connections_.end(), target);
    if (it == connections_.end()) return false;
    erase_connection_at_locked_(static_cast<std::size_t>(
        std::distance(connections_.begin(), it)));
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::disconnect_modulation(NodeId source, PortIndex source_port, NodeId dest,
                                        uint32_t dest_param_id, bool audio_rate) {
    GraphMutationLock mutation_lock(*this);
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const auto it =
        std::find_if(connections_.begin(), connections_.end(), [&](const Connection& connection) {
            return connection.source_node == source && connection.source_port == source_port &&
                   connection.dest_node == dest &&
                   connection.automation_param_id == dest_param_id &&
                   connection.automation == !audio_rate &&
                   connection.audio_rate_modulation == audio_rate;
        });
    if (it == connections_.end())
        return false;
    erase_connection_at_locked_(static_cast<std::size_t>(std::distance(connections_.begin(), it)));
    invalidate_live_locked_();
    return true;
}

// Does not lock: scans nodes_; the caller MUST hold graph_mutation_mutex_
// (every internal mutator/reader below does). Public direct callers
// (e.g. nodes()/node() external users) own their serialization per the accessor
// contract documented on graph_mutation_mutex_.
const GraphNode* SignalGraph::node(NodeId id) const {
    for (auto& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}

GraphNode* SignalGraph::node_mut_locked_(NodeId id) {
    assert_graph_mutation_locked_();
    return const_cast<GraphNode*>(node(id));
}

void SignalGraph::append_connection_locked_(Connection connection) {
    assert_graph_mutation_locked_();
    const auto new_size = connections_.size() + 1;
    // Reserve both arrays before changing either size. If allocation fails the
    // graph remains unchanged instead of leaving identity metadata misaligned.
    connections_.reserve(new_size);
    connection_identities_.reserve(new_size);
    connections_.push_back(std::move(connection));
    connection_identities_.push_back(next_connection_identity_);
    ++next_connection_identity_;
    assert(connections_.size() == connection_identities_.size());
}

void SignalGraph::erase_connection_at_locked_(std::size_t index) {
    assert_graph_mutation_locked_();
    assert(index < connections_.size());
    assert(connections_.size() == connection_identities_.size());
    connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(index));
    connection_identities_.erase(
        connection_identities_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool SignalGraph::has_path_locked_(NodeId from, NodeId to) const {
    std::unordered_set<NodeId> visited;
    std::queue<NodeId> queue;
    queue.push(from);
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        if (current == to) return true;
        if (visited.count(current)) continue;
        visited.insert(current);
        for (auto& c : connections_) {
            if (c.feedback) continue;
            if (c.source_node == current) queue.push(c.dest_node);
        }
    }
    return false;
}

bool SignalGraph::would_create_cycle(NodeId source, NodeId dest) const {
    return has_path_locked_(dest, source);
}

namespace {
std::vector<NodeId> processing_order_for(const std::vector<GraphNode>& nodes,
                                         const std::vector<Connection>& connections) {
    std::unordered_map<NodeId, int> in_degree;
    for (const auto& n : nodes)
        in_degree[n.id] = 0;
    for (const auto& c : connections) {
        if (c.feedback) continue;
        in_degree[c.dest_node]++;
    }
    std::queue<NodeId> queue;
    for (auto& [id, deg] : in_degree) if (deg == 0) queue.push(id);
    std::vector<NodeId> order;
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        order.push_back(current);
        for (const auto& c : connections) {
            if (c.feedback) continue;
            // Automation edges DO contribute to topological order — the
            // source must be processed before the dest so its output
            // buffer is valid when we sample it for param events.
            if (c.source_node == current) {
                if (--in_degree[c.dest_node] == 0) queue.push(c.dest_node);
            }
        }
    }
    return order;
}
} // namespace

std::vector<NodeId> SignalGraph::processing_order() const {
    return processing_order_for(nodes_, connections_);
}

bool SignalGraph::set_node_parameter(NodeId id, uint32_t param_id, float value) {
    // Scans nodes_ via node(); serialize against a concurrent mutator/prepare.
    // Forwards to the plugin slot's own (independently synchronized) parameter
    // store — no GraphNode plain field is written here.
    GraphMutationLock mutation_lock(*this);
    if (sample_region_for_node_locked_(id) != 0)
        return false;
    auto* n = node_mut_locked_(id);
    if (!n || n->type != NodeType::Plugin || !n->plugin) return false;
    n->plugin->set_parameter(param_id, value);
    return true;
}

float SignalGraph::get_node_parameter(NodeId id, uint32_t param_id) const {
    // Scans nodes_ via node(); serialize against a concurrent mutator/prepare.
    GraphMutationLock mutation_lock(*this);
    auto* n = node(id);
    if (!n || n->type != NodeType::Plugin || !n->plugin) return 0.f;
    return n->plugin->get_parameter(param_id);
}

int SignalGraph::node_latency_samples(NodeId id) const {
    // Pin the live snapshot for the whole dereference (see inject_midi).
    auto read_guard = live_slot_.read();
    const auto* cg = read_guard.get();
    if (!cg) return 0;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end()) return 0;
    return (int)it->second.input_latency;
}

SignalGraph::PreparedStats SignalGraph::prepared_stats() const {
    return PreparedStats{
        .node_count = prepared_node_count_.load(std::memory_order_relaxed),
        .ordered_node_count =
            prepared_ordered_node_count_.load(std::memory_order_relaxed),
        .connection_count =
            prepared_connection_count_.load(std::memory_order_relaxed),
        .total_ports = prepared_total_ports_.load(std::memory_order_relaxed),
        .max_block_size = prepared_max_block_size_.load(std::memory_order_relaxed),
        .node_audio_buffer_bytes =
            prepared_node_audio_buffer_bytes_.load(std::memory_order_relaxed),
        .automation_buffer_bytes =
            prepared_automation_buffer_bytes_.load(std::memory_order_relaxed),
        .delay_buffer_bytes =
            prepared_delay_buffer_bytes_.load(std::memory_order_relaxed),
        .total_prepared_buffer_bytes =
            prepared_total_buffer_bytes_.load(std::memory_order_relaxed),
    };
}

std::uint64_t SignalGraph::sample_region_binding_generation() const noexcept {
    auto read_guard = live_slot_.read();
    const auto* cg = read_guard.get();
    return cg != nullptr && cg->sample_region_bank ? cg->sample_region_bank->binding_generation()
                                                   : 0;
}

std::vector<SampleRegionRuntimeReceipt> SignalGraph::sample_region_runtime_receipts() const {
    auto read_guard = live_slot_.read();
    const auto* cg = read_guard.get();
    std::vector<SampleRegionRuntimeReceipt> receipts;
    if (cg == nullptr)
        return receipts;
    receipts.reserve(cg->sample_regions.size());
    for (const auto& region : cg->sample_regions) {
        if (region)
            receipts.push_back(region->receipt());
    }
    return receipts;
}

int SignalGraph::prepared_max_block_size() const noexcept {
    return live_slot_.live() ? live_slot_.live()->max_block_size : 0;
}

std::atomic<float>* SignalGraph::live_gain_atomic(NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->runtime.find(id);
    if (it == live_slot_.live()->runtime.end()) return nullptr;
    return it->second.gain.get();
}

PluginSlot* SignalGraph::live_plugin_slot(NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->plugins.find(id);
    if (it == live_slot_.live()->plugins.end()) return nullptr;
    return it->second.get();
}

const CustomNodeProcessFn* SignalGraph::live_custom_processor(NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->custom_processors.find(id);
    if (it == live_slot_.live()->custom_processors.end()) return nullptr;
    return &it->second;
}

const CustomNodeTransportProcessFn* SignalGraph::live_custom_transport_processor(
    NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->custom_transport_processors.find(id);
    if (it == live_slot_.live()->custom_transport_processors.end()) return nullptr;
    return &it->second;
}

const CustomNodeEventProcessFn* SignalGraph::live_custom_event_processor(
    NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->custom_event_processors.find(id);
    if (it == live_slot_.live()->custom_event_processors.end()) return nullptr;
    return &it->second;
}

int SignalGraph::live_custom_latency_samples(NodeId id) const noexcept {
    // Read-guarded rather than a bare live() deref: this one is read by the
    // PDC/bake paths while a re-prepare can be publishing a new snapshot, and
    // the guard keeps the map alive for the lookup.
    auto read_guard = live_slot_.read();
    const auto* cg = read_guard.get();
    if (cg == nullptr) return 0;
    auto it = cg->custom_latency_samples.find(id);
    return it == cg->custom_latency_samples.end() ? 0 : it->second;
}

const CustomNodeParamProcessFn* SignalGraph::live_custom_param_processor(
    NodeId id) const noexcept {
    if (!live_slot_.live()) return nullptr;
    auto it = live_slot_.live()->custom_param_processors.find(id);
    if (it == live_slot_.live()->custom_param_processors.end()) return nullptr;
    return &it->second;
}

std::shared_ptr<const void> SignalGraph::live_snapshot_handle() const noexcept {
    return live_slot_.live();  // aliases the live CompiledGraph as an opaque keepalive
}

SignalGraph::RoutedExecutionStatus
SignalGraph::routed_execution_status(int block_size) const noexcept {
    RoutedExecutionStatus status;
    const auto live = live_slot_.live();
    if (!live || block_size <= 0)
        return status;

    status.prepared = true;
    status.serial_snapshot_valid = live->routed.serial.valid;
    status.serial_pool_fits =
        live->routed.serial.valid &&
        live->routed.serial.pool.fits(live->routed.serial.snapshot,
                                      static_cast<std::uint32_t>(block_size));
    status.parallel_snapshot_valid = live->routed.parallel.valid;
    status.parallel_pool_fits =
        live->routed.parallel.valid &&
        live->routed.parallel.pool.fits(live->routed.parallel.snapshot,
                                        static_cast<std::uint32_t>(block_size));
    status.worker_pool_running = worker_pool_.running();
    status.reference_walk_permitted =
        routed_only_execution_owners_.load(std::memory_order_relaxed) == 0;

    switch (live->pdc_execution_domain) {
    case PdcExecutionDomain::Legacy:
        break;
    case PdcExecutionDomain::RoutedSerial:
        status.serial_selected = true;
        break;
    case PdcExecutionDomain::RoutedParallel:
        status.parallel_selected = true;
        break;
    case PdcExecutionDomain::Dynamic:
        status.serial_selected =
            canonical_executor_routing_enabled_.load(std::memory_order_relaxed);
        status.parallel_selected =
            parallel_routing_enabled_.load(std::memory_order_relaxed);
        break;
    }
    return status;
}

pulp::audio::AudioProcessLoadSnapshot SignalGraph::graph_load() const {
    return graph_load_ ? graph_load_->snapshot() : pulp::audio::AudioProcessLoadSnapshot{};
}

void SignalGraph::set_live_dsp_telemetry_enabled(bool enabled) {
    // Serialize the desired state + live reflection with snapshot publication.
    // Otherwise a prepared transaction could read the new desired value while
    // this call still updates the old snapshot, or publish a candidate seeded
    // before this call. Pinning keeps the selected snapshot alive; the brief
    // lock -> pin order is safe because release() must acquire this same lock
    // before it can wait for readers.
    GraphMutationLock mutation_lock(*this);
    desired_live_dsp_telemetry_enabled_.store(enabled, std::memory_order_relaxed);
    auto read_guard = live_slot_.read();
    if (auto* cg = read_guard.get()) {
        cg->live_dsp_telemetry.set_enabled(enabled);
    }
}

bool SignalGraph::live_dsp_telemetry_enabled() const {
    return desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed);
}

pulp::audio::LiveDspTelemetrySnapshot SignalGraph::poll_live_dsp_telemetry() {
    // Single non-real-time poller (control/UI thread): pin the live snapshot, drain
    // its ring (the SPSC consumer side; the audio thread is the producer), and return
    // a copy of the latest summary while the snapshot is still pinned alive.
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (!cg) return {};
    cg->live_dsp_telemetry.drain();
    return cg->live_dsp_telemetry.latest();
}

std::vector<SignalGraph::NodeLoadReport> SignalGraph::node_loads() const {
    // Control/UI-thread read of the persistent per-node measurers. node_load_
    // is only mutated on the control thread (compile_), so this is race-free
    // against topology recompiles; the audio thread writes only the measurer
    // objects' relaxed atomics, which snapshot() reads coherently.
    // Filter to currently-present nodes so removed nodes' lingering measurers
    // don't surface as phantom reports. The measurers are intentionally NOT
    // erased from node_load_ (it is insert-only — see compile_): a
    // retired-but-not-yet-drained snapshot may still hold raw
    // NodeRuntime::load pointers into them, so erasing here would risk a
    // use-after-free on the draining audio thread. Residual map growth is
    // bounded by the number of distinct NodeIds the graph has ever held.
    // The nodes_ scan is serialized under graph_mutation_mutex_ (against a
    // concurrent mutator/prepare); take it FIRST and release it before
    // node_load_mu_ so the lock order is graph_mutation_mutex_ -> node_load_mu_,
    // matching compile_() (which holds graph_mutation_mutex_ via prepare() and
    // takes node_load_mu_ inside). live_ids is a private copy, so it is safe to
    // use after releasing graph_mutation_mutex_.
    std::unordered_set<NodeId> live_ids;
    {
        GraphMutationLock mutation_lock(*this);
        live_ids.reserve(nodes_.size());
        for (const auto& n : nodes_) live_ids.insert(n.id);
    }

    std::lock_guard<std::mutex> node_load_lock(node_load_mu_);
    std::vector<NodeLoadReport> reports;
    reports.reserve(node_load_.size());
    for (const auto& [id, measurer] : node_load_) {
        if (measurer && live_ids.count(id) != 0) {
            reports.push_back(NodeLoadReport{id, measurer->snapshot()});
        }
    }
    return reports;
}

SignalGraph::RuntimeBudgetReport
SignalGraph::evaluate_optional_runtime_budget(
    runtime::RuntimeBudgetFrame& frame,
    runtime::RuntimeWorkLane lane,
    bool required) const noexcept {
    const auto stats = prepared_stats();
    std::uint64_t estimated = 0;
    estimated = saturating_add_u64(
        estimated,
        saturating_mul_u64(static_cast<std::uint64_t>(stats.node_count), 16));
    estimated = saturating_add_u64(
        estimated,
        saturating_mul_u64(static_cast<std::uint64_t>(stats.connection_count), 8));
    if (stats.max_block_size > 0) {
        estimated = saturating_add_u64(
            estimated,
            saturating_mul_u64(
                static_cast<std::uint64_t>(stats.total_ports),
                static_cast<std::uint64_t>(stats.max_block_size)));
    }
    estimated = saturating_add_u64(
        estimated,
        static_cast<std::uint64_t>(
            stats.total_prepared_buffer_bytes / sizeof(float)));

    const auto decision = frame.evaluate(lane, estimated, required);
    return {
        .decision = decision,
        .frame_stats = frame.stats(),
        .estimated_cost = estimated,
        .prepared = stats.node_count != 0,
    };
}

void SignalGraph::release() {
    // Serialize against concurrent control-thread mutators / prepare() for the
    // same two surfaces prepare() guards: the nodes_ iteration below and the
    // snapshot-publication state (live_slot_).
    // Slot::wait_and_clear() blocks on the reader count while holding this
    // mutex; that is deadlock-free because the only thread that holds a reader pin
    // AND wants this mutex (set_node_gain) releases the mutex before pinning, and
    // the pure-snapshot readers (inject_midi / extract_midi / node_latency_samples)
    // never take this mutex at all.
    GraphMutationLock mutation_lock(*this);
    ++authoring_generation_;

    cancel_swap_edit_locked_();

    live_slot_.unpublish();
    live_slot_.wait_and_clear();

    for (auto& n : nodes_) if (n.plugin) n.plugin->release();
    for (auto& [_, processor] : processor_nodes_) {
        if (processor && processor->instance)
            (void)processor->instance->release();
    }
    // Release stateful custom instances on the UI thread, mirroring the plugin
    // release above. The instance object stays alive until its snapshots also
    // drop; release() just lets the type free scratch.
    for (auto& n : nodes_) {
        if (n.type != NodeType::Custom || !n.custom_instance) continue;
        if (const auto* type =
                custom_node_type(n.custom_type_id, n.custom_type_version);
            type && type->release) {
            type->release(n.custom_instance.get());
        }
    }
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_locked_();
}

std::vector<uint8_t> SignalGraph::custom_node_state(NodeId id) const {
    // Reads nodes_ + GraphNode custom fields; serialize against a concurrent
    // mutator/prepare. custom_node_type() is a lock-free helper used under it.
    GraphMutationLock mutation_lock(*this);
    for (const auto& n : nodes_) {
        if (n.id != id) continue;
        if (n.type != NodeType::Custom) return {};
        // Prefer the live instance's current state; fall back to the stored blob
        // (e.g. unresolved nodes, or before the first prepare()).
        if (n.custom_instance) {
            if (const auto* type =
                    custom_node_type(n.custom_type_id, n.custom_type_version);
                type && type->save_state) {
                return type->save_state(n.custom_instance.get());
            }
        }
        return n.custom_state_blob;
    }
    return {};
}

bool SignalGraph::set_custom_node_state(NodeId id,
                                        const std::vector<uint8_t>& bytes) {
    // Writes GraphNode custom fields + invalidate_live_locked_(); serialize against a
    // concurrent mutator/prepare.
    GraphMutationLock mutation_lock(*this);
    if (sample_region_for_node_locked_(id) != 0)
        return false;
    cancel_swap_edit_locked_();
    for (auto& n : nodes_) {
        if (n.id != id) continue;
        if (n.type != NodeType::Custom) return false;
        n.custom_state_blob = bytes;
        // Apply to the live instance on the next prepare() (one-shot). The blob
        // is retained regardless, so it survives even when the type is
        // unresolved and is re-emitted on the next serialize.
        n.custom_state_pending = true;
        invalidate_live_locked_();
        return true;
    }
    return false;
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input,
                          int num_samples) {
    process_impl(output, input, num_samples, /*transport=*/nullptr);
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input,
                          int num_samples,
                          const format::ProcessContext& transport) {
    process_impl(output, input, num_samples, &transport);
}

void SignalGraph::process_impl(audio::BufferView<float>& output,
                               const audio::BufferView<const float>& input,
                               int num_samples,
                               const format::ProcessContext* transport) {
    // See runtime::Slot: its reader count and the raw snapshot
    // pointer form one RCU-style lifetime handshake. Slot::ReadGuard is a
    // private nested struct (signal_graph.hpp) so the control-thread snapshot
    // readers can pin the same way.
    auto read_guard = live_slot_.read();
    process_snapshot_impl(output, input, num_samples, transport, read_guard.get());
}

void SignalGraph::process_snapshot_impl(audio::BufferView<float>& output,
                                        const audio::BufferView<const float>& input,
                                        int num_samples,
                                        const format::ProcessContext* transport,
                                        CompiledGraph* cg) {
    // Negative or zero block sizes mean "nothing to do" — return without
    // touching output (a memset with size_t(negative) wraps to a huge size).
    if (num_samples <= 0) return;
    if (!cg || num_samples > cg->max_block_size) {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            std::memset(output.channel_ptr(c), 0,
                        sizeof(float) * static_cast<size_t>(num_samples));
        if (routed_only_execution_owners_.load(std::memory_order_relaxed) != 0)
            routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    SampleRegionExecutionDomain::Admission sample_region_admission;
    if (cg->sample_region_bank) {
        sample_region_admission = cg->sample_region_bank->domain().try_admit(
            cg->sample_region_bank->binding_generation());
        // Contention and stale-snapshot re-entry fail without touching retained
        // state cells. The caller still receives a deterministic silent block.
        if (!sample_region_admission) {
            for (std::size_t c = 0; c < output.num_channels(); ++c)
                std::fill_n(output.channel_ptr(c), num_samples, 0.0f);
            routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (transport != nullptr && transport->reset_requested)
            cg->sample_region_bank->reset();
    }

    // Bracket the whole block with the graph-level load measurer (RT-safe: begin()/
    // end() are relaxed-atomic timestamps, no alloc/lock). The RAII guard's end()
    // covers every return path below. graph_load() reads this for live-swap admission.
    if (graph_load_) graph_load_->begin(num_samples, static_cast<float>(cg->sample_rate));

    // Record one live-DSP telemetry block per process() call, path-agnostic: the
    // routed executor and the legacy walk BOTH time each node into its persistent
    // AudioProcessLoadMeasurer (node_load_) and the whole block into graph_load_.
    // This guard reads those already-populated measurers after the block and pushes
    // a single fixed-slot record — so per-node p50/p95/p99 + jitter + over-budget
    // attribution work on every execution path with no per-node hook in either the
    // executor or the walk. Declared BEFORE graph_load_end_guard so it destructs
    // AFTER it (reverse order): graph_load_->end() has stamped this block's graph
    // elapsed by the time this runs. Inactive at one-branch cost when telemetry is
    // off; the record path is allocation-free (pre-sized scratch + drop-on-full ring).
    struct TelemetryRecordGuard {
        CompiledGraph* cg;
        audio::AudioProcessLoadMeasurer* graph_measurer;
        int num_samples;
        ~TelemetryRecordGuard() {
            auto& store = cg->live_dsp_telemetry;
            if (!store.enabled() || !store.prepared()) return;
            std::int64_t* scratch = store.external_record_scratch();
            if (scratch == nullptr) return;
            const std::uint32_t n = store.node_count();
            for (std::uint32_t i = 0; i < n && i < cg->ordered_runtime.size(); ++i) {
                auto* rt = cg->ordered_runtime[i].runtime;
                auto* m = rt ? rt->load : nullptr;
                scratch[i] = m ? m->last_elapsed_ns() : 0;
            }
            const std::int64_t graph_ns =
                graph_measurer ? graph_measurer->last_elapsed_ns() : 0;
            store.inject_block(std::span<const std::int64_t>(scratch, n), graph_ns,
                               static_cast<std::uint32_t>(num_samples), cg->sample_rate);
        }
    } telemetry_record_guard{cg, graph_load_.get(), num_samples};

    struct GraphLoadEndGuard {
        audio::AudioProcessLoadMeasurer* m;
        ~GraphLoadEndGuard() {
            if (m) m->end();
        }
    } graph_load_end_guard{graph_load_.get()};

    // Routed dispatch (opt-in): try the levelized PARALLEL executor first (if
    // enabled), then the SERIAL executor, then fall through to the legacy walk.
    // Both routed paths run GraphRuntimeExecutor (which zeroes the output bus and
    // accumulates AudioOutput itself, so a successful routed call returns before
    // the legacy zero+walk below) and SHARE the MIDI mailbox bridge (the MIDI
    // scratch + MidiInput/Output node lists are shared — identical plan). The
    // dispatch stays inside the Slot reader-pin, so `cg` (snapshots, pools, gain
    // atomics) is pinned for the whole call. Output is bit-identical across paths.
    {
        const auto frames32 = static_cast<std::uint32_t>(num_samples);
        const bool has_midi = cg->routed.midi.node_count() > 0;
        const bool has_automation = cg->routed.automation.node_count() > 0;

        pulp::format::BusBufferSet buses;
        const bool buses_ok =
            buses.add_input("main", input, pulp::format::BusRole::Main) &&
            buses.add_output("main", output, pulp::format::BusRole::Main);
        if (buses_ok) {
            // Anticipation safety (per-node, not blanket): a transport-sensitive
            // node opts in via GraphNode::transport_sensitive, which seeds
            // AnticipationExclusion::TransportSensitive and so excludes that node
            // (and its downstream cone) from the ahead-rendered interior. Every
            // node that IS ahead-rendered is therefore transport-insensitive by
            // construction and ignores block.transport. So the live transport can
            // stay populated even while anticipation is active: forwarding it is
            // inert for the masked interior, and the transport-sensitive nodes
            // that need it always run live/exterior. (Resolved at compile; the
            // count of nodes forced exterior is in
            // transport_suppressed_for_anticipation().)
            pulp::format::ProcessBlock block;
            block.sample_rate = cg->sample_rate;
            block.frame_count = frames32;
            block.buses = &buses;
            if (transport != nullptr) {
                // Deliver transport + process_mode + render_speed_hint to
                // Processors via *block.transport. block.sample_rate stays sourced
                // from cg (the prepared rate is authoritative). render_speed is left
                // at 1.0: render_speed_hint is a categorical hint that reaches
                // Processors through *block.transport, not a numeric multiplier.
                block.transport = transport;
                block.mode = transport->process_mode;
            }

            // Run one routed path with the shared MIDI mailbox bridge around it.
            // `run` returns the executor result; this returns true iff routing
            // succeeded (took the path). On failure the consumed MIDI sequences
            // are NOT committed, so a fallback path re-consumes the same block.
            auto dispatch_routed = [&](auto&& run) -> bool {
                if (!block.validate()) return false;
                reset_plugin_parameter_event_sequences(cg->routed.serial.plugin_ctx);
                reset_plugin_parameter_event_sequences(
                    cg->routed.parallel.plugin_ctx);
                for (auto& processor : cg->routed.processor_parameter_inputs) {
                    processor.pending_seq = 0;
                    auto* queue = cg->routed.automation.events(processor.plan_index);
                    auto runtime_it = cg->runtime.find(processor.id);
                    if (queue == nullptr || runtime_it == cg->runtime.end())
                        continue;
                    queue->clear();
                    const auto pending =
                        append_parameter_mailbox_events_(&runtime_it->second, *queue);
                    processor.pending_seq = pending.exact;
                }
                if (has_midi) {
                    for (auto& mi : cg->routed.midi_inputs) {
                        mi.pending_seq = 0;
                        midi::MidiBuffer* out_buf = cg->routed.midi.out(mi.plan_index);
                        if (out_buf == nullptr) continue;
                        clear_midi_block(*out_buf);
                        cg->routed.midi.set_out_incomplete(mi.plan_index, false);
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it == cg->runtime.end() ||
                            !rt_it->second.midi_input_mailbox) {
                            continue;
                        }
                        const auto& injected =
                            rt_it->second.midi_input_mailbox->published.read();
                        const auto sequence_seen =
                            rt_it->second.midi_input_mailbox->sequence_seen.load(
                                std::memory_order_relaxed);
                        if (injected.sequence != 0 &&
                            injected.sequence != sequence_seen) {
                            cg->routed.midi.set_out_incomplete(
                                mi.plan_index, !injected.copy_to_midi(*out_buf));
                            mi.pending_seq = injected.sequence;
                        }
                    }
                }
                if (!run().ok()) return false;
                commit_plugin_parameter_event_sequences(cg->routed.serial.plugin_ctx);
                commit_plugin_parameter_event_sequences(
                    cg->routed.parallel.plugin_ctx);
                for (const auto& processor : cg->routed.processor_parameter_inputs) {
                    if (processor.pending_seq == 0)
                        continue;
                    auto runtime_it = cg->runtime.find(processor.id);
                    if (runtime_it != cg->runtime.end() &&
                        runtime_it->second.exact_parameter_input_mailbox) {
                        runtime_it->second.exact_parameter_input_mailbox->sequence_seen.store(
                            processor.pending_seq, std::memory_order_relaxed);
                    }
                }
                if (has_midi) {
                    for (const auto& mi : cg->routed.midi_inputs) {
                        if (mi.pending_seq == 0) continue;
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it != cg->runtime.end() &&
                            rt_it->second.midi_input_mailbox) {
                            rt_it->second.midi_input_mailbox->sequence_seen.store(
                                mi.pending_seq, std::memory_order_relaxed);
                        }
                    }
                    for (const auto& mo : cg->routed.midi_outputs) {
                        midi::MidiBuffer* in_buf = cg->routed.midi.in(mo.plan_index);
                        auto rt_it = cg->runtime.find(mo.id);
                        if (in_buf == nullptr || rt_it == cg->runtime.end() ||
                            !rt_it->second.midi_output_mailbox) {
                            continue;
                        }
                        cg->midi_publish_scratch.set_from_midi(
                            *in_buf, 0, cg->routed.midi.in_incomplete(mo.plan_index));
                        if (cg->midi_publish_scratch.has_payload()
                            && !rt_it->second.midi_output_mailbox->pending.try_push(
                                cg->midi_publish_scratch)) {
                            rt_it->second.midi_output_mailbox->incomplete.store(
                                true, std::memory_order_relaxed);
                        }
                    }
                }
                return true;
            };

            // worker_pool_.running() is a plain flag read here; it is safe without
            // a drain handshake only because running_ is flipped false exactly
            // once, by ~GraphRuntimeWorkerPool, when no audio-thread reader exists
            // (see the compile_ start invariant). Each dispatch is idempotent: it
            // re-clears the MIDI ingress buffers and does not commit consumed
            // sequences until run() succeeds, and every executor zeroes the output
            // bus before accumulating — so a parallel attempt that returns false
            // (a node failed, or it does not fit) can fall through to the serial
            // executor and then the legacy walk re-rendering the same block with
            // no double-consumed MIDI and no doubled output.
            // Anticipative rendering (tried first): the lane has pre-rendered the
            // interior off the audio thread. Consume one block, fill the interior
            // boundary-source output slots with it (or zero them on an underrun /
            // block-size mismatch — NEVER re-run the interior, whose plugin state
            // the producer owns), then run the routed walk with the interior masked
            // so only the exterior runs live. Uses the serial routed snapshot.
            if (anticipation_enabled_.load(std::memory_order_relaxed) &&
                cg->anticipation.valid &&
                cg->anticipation.lane.output_channels() ==
                    cg->anticipation.prefill.size()) {
                // STRUCTURALLY TERMINAL once anticipation is valid: the interior's
                // plugin state is advanced solely by the producer (pump_anticipation),
                // so the live path must NEVER run the interior — not via a fallback,
                // and not even when the pool can't fit the block. The fits() check is
                // INSIDE the branch (not an entry gate) precisely so a future change
                // that made it false could not let control fall through to the
                // parallel/legacy paths below, which would run the masked interior
                // live and double-advance producer-owned state. Worst case here is a
                // silent block, never a double-render.
                bool ok = false;
                if (cg->routed.serial.pool.fits(cg->routed.serial.snapshot, frames32)) {
                    const bool size_ok =
                        frames32 == static_cast<std::uint32_t>(
                                        cg->anticipation.lane.block_frames());
                    bool hit = false;
                    if (size_ok) {
                        pulp::audio::BufferView<float> cap(
                            cg->anticipation.consume_ptrs.data(),
                            cg->anticipation.consume_ptrs.size(), frames32);
                        hit = cg->anticipation.lane.consume(cap);
                    }
                    for (const auto& pf : cg->anticipation.prefill) {
                        float* dst = cg->routed.serial.pool.slot_data(pf.slot);
                        if (dst == nullptr) continue;
                        if (hit) {
                            std::copy_n(cg->anticipation.consume_ptrs[pf.out_channel],
                                        num_samples, dst);
                        } else {
                            std::fill_n(dst, num_samples, 0.0f);
                        }
                    }
                    ok = dispatch_routed([&] {
                        return executor_.process_routed(
                            block, cg->routed.serial.snapshot, cg->routed.serial.pool,
                            has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr, {}, {},
                            {}, {}, cg->anticipation.skip_mask);
                    });
                }
                if (!ok) {
                    for (std::size_t c = 0; c < output.num_channels(); ++c) {
                        std::memset(output.channel_ptr(c), 0,
                                    sizeof(float) * static_cast<std::size_t>(num_samples));
                    }
                }
                return;
            }

            // Reached only when anticipation is NOT valid (the branch above is
            // terminal whenever it is) — REQUIRED for safety: the parallel and
            // legacy paths run every node, including any interior the producer
            // owns, so they must never execute while anticipation is active.
            // Observability for a SILENT degradation the parity test cannot see:
            // when a routed path is ELIGIBLE (enabled + a valid routed snapshot +
            // the pool fits) but its dispatch returns false, control falls through
            // to the legacy walk. Because the walk is BOTH the oracle and the
            // fallback, that produces no divergence — so an eligible graph that
            // stopped routing would be invisible. Flag it: count it (relaxed,
            // RT-safe) and warn in debug builds. A normal fallback (routing
            // disabled, ineligible, or build failure → routed.serial.valid false) does
            // NOT set this, so the counter isolates the should-have-routed case.
            bool routed_eligible_dispatch_failed = false;

            const bool use_parallel =
                cg->pdc_execution_domain == PdcExecutionDomain::RoutedParallel ||
                (cg->pdc_execution_domain == PdcExecutionDomain::Dynamic &&
                 parallel_routing_enabled_.load(std::memory_order_relaxed));
            const bool use_serial =
                !cg->processors.empty() ||
                cg->pdc_execution_domain == PdcExecutionDomain::RoutedSerial ||
                (cg->pdc_execution_domain == PdcExecutionDomain::Dynamic &&
                 canonical_executor_routing_enabled_.load(std::memory_order_relaxed));

            if (use_parallel &&
                cg->routed.parallel.valid && worker_pool_.running() &&
                cg->routed.parallel.pool.fits(cg->routed.parallel.snapshot, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_parallel(
                            block, cg->routed.parallel.snapshot,
                            cg->routed.parallel.levelization, cg->routed.parallel.pool,
                            worker_pool_, has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr);
                    })) {
                    return;
                }
                routed_eligible_dispatch_failed = true;  // eligible but did not route
            }

            if (use_serial &&
                cg->routed.serial.valid &&
                cg->routed.serial.pool.fits(cg->routed.serial.snapshot, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_routed(
                            block, cg->routed.serial.snapshot, cg->routed.serial.pool,
                            has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr);
                    })) {
                    return;
                }
                routed_eligible_dispatch_failed = true;  // eligible but did not route
            }

            if (routed_eligible_dispatch_failed &&
                routed_only_execution_owners_.load(std::memory_order_relaxed) == 0) {
                const std::uint64_t prev =
                    routed_walk_fallbacks_.fetch_add(1, std::memory_order_relaxed);
                (void)prev;
#ifndef NDEBUG
                // Warn ONCE per graph (the counter carries the full tally) — the
                // log path is not RT-safe, so don't repeat it every block.
                if (prev == 0) {
                    runtime::log_warn(
                        "SignalGraph: routed dispatch failed for an eligible graph; "
                        "falling back to the legacy walk (see routed_walk_fallbacks())");
                }
#endif
            }
        }
        // Any setup failure / disabled path falls through to the legacy walk.
    }

    // Authored Processor nodes have only the routed ProcessBlock binding. Never
    // reinterpret one as the unresolved-plugin pass-through used by the legacy
    // reference walk; a routed failure is a fail-closed silent block.
    if (!cg->processors.empty()) {
        output.clear();
        routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (routed_only_execution_owners_.load(std::memory_order_relaxed) != 0) {
        output.clear();
        routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // No routed path took this block — run the legacy serial reference walk,
    // the hand-maintained bit-exact oracle/fallback (see
    // signal_graph_reference_walk.cpp).
    run_reference_walk_(output, input, num_samples, cg);
}

bool SignalGraph::ExecutionSnapshot::inject_midi(
    NodeId midi_input_node, const midi::MidiBuffer& events) const noexcept {
    return snapshot_ != nullptr &&
           SignalGraph::inject_midi_into_snapshot_(*snapshot_, midi_input_node, events);
}

std::uint64_t SignalGraph::ExecutionSnapshot::sample_region_binding_generation() const noexcept {
    return snapshot_ != nullptr && snapshot_->sample_region_bank
               ? snapshot_->sample_region_bank->binding_generation()
               : 0;
}

std::vector<SampleRegionRuntimeReceipt>
SignalGraph::ExecutionSnapshot::sample_region_runtime_receipts() const {
    std::vector<SampleRegionRuntimeReceipt> receipts;
    if (snapshot_ == nullptr)
        return receipts;
    receipts.reserve(snapshot_->sample_regions.size());
    for (const auto& region : snapshot_->sample_regions) {
        if (region)
            receipts.push_back(region->receipt());
    }
    return receipts;
}

bool SignalGraph::ExecutionSnapshot::inject_parameter_events(
    NodeId plugin_node,
    const state::ParameterEventQueue& events) const noexcept {
    return snapshot_ != nullptr &&
           SignalGraph::inject_parameter_events_into_snapshot_(
               *snapshot_, plugin_node, events);
}

bool SignalGraph::ExecutionSnapshot::inject_exact_parameter_events(
    NodeId plugin_node, const state::ParameterEventQueue& events,
    SnapshotParameterIngressPasskey) const noexcept {
    return snapshot_ != nullptr &&
           SignalGraph::inject_exact_parameter_events_into_snapshot_(
               *snapshot_, plugin_node, events);
}

void SignalGraph::ExecutionSnapshot::process(
    audio::BufferView<float>& output, const audio::BufferView<const float>& input,
    int num_samples) const noexcept {
    if (owner_ == nullptr || snapshot_ == nullptr) {
        if (num_samples > 0) output.clear();
        return;
    }
    owner_->process_snapshot_impl(output, input, num_samples, nullptr, snapshot_.get());
}

void SignalGraph::ExecutionSnapshot::process(
    audio::BufferView<float>& output, const audio::BufferView<const float>& input,
    int num_samples, const format::ProcessContext& transport) const noexcept {
    if (owner_ == nullptr || snapshot_ == nullptr) {
        if (num_samples > 0) output.clear();
        return;
    }
    owner_->process_snapshot_impl(output, input, num_samples, &transport, snapshot_.get());
}

void SignalGraph::clear() {
    // Wipes nodes_/connections_ + invalidate_live_locked_(); serialize against a
    // concurrent mutator/prepare. invalidate_live_locked_() drives only the non-blocking
    // prune, so holding the mutex across it is deadlock-free.
    GraphMutationLock mutation_lock(*this);
    cancel_swap_edit_locked_();
    connections_.clear();
    connection_identities_.clear();
    nodes_.clear();
    processor_nodes_.clear();
    sample_region_definitions_.clear();
    sample_region_parameter_binding_ = nullptr;
    prepared_sample_region_bank_.reset();
    prepared_sample_regions_.clear();
    next_id_ = 1;
    invalidate_live_locked_();
}

bool SignalGraph::set_node_gain(NodeId id, float linear_gain) {
    // Write the UI-thread-owned scalar on GraphNode so it survives future
    // compile_() calls. Also reflect into the live snapshot's runtime through
    // a per-runtime atomic so the change takes effect without a re-prepare.
    //
    // The GraphNode::gain write + the node() scan of nodes_ are serialized under
    // graph_mutation_mutex_ against compile_()'s read of GraphNode::gain and its
    // nodes_ iteration (this API runs on the UI thread, prepare()/compile_() on a
    // host thread). The lock is RELEASED before the Slot-pinned
    // snapshot reflection below, so it never nests inside the reader-pin / RCU
    // drain mechanism and cannot invert lock order with release()'s reader wait.
    {
        GraphMutationLock mutation_lock(*this);
        if (sample_region_for_node_locked_(id) != 0)
            return false;
        auto* n = node_mut_locked_(id);
        if (!n) return false;
        n->gain = linear_gain;
        ++authoring_generation_;
    }
    // Pin the live snapshot around the load + the per-runtime gain store: this
    // UI-thread-owned API is not the prepare/release thread, so without the
    // guard a concurrent prepare()/release() could retire+free `cg` between the
    // load and the store (use-after-free).
    auto read_guard = live_slot_.read();
    auto* cg = read_guard.get();
    if (cg) {
        auto it = cg->runtime.find(id);
        if (it != cg->runtime.end() && it->second.gain) {
            it->second.gain->store(linear_gain, std::memory_order_relaxed);
        }
    }
    return true;
}

float SignalGraph::node_gain(NodeId id) const {
    // Read counterpart of set_node_gain(): the node() scan of nodes_ and the
    // GraphNode::gain read are serialized under graph_mutation_mutex_ against
    // compile_()'s nodes_ iteration / gain read and concurrent set_node_gain().
    GraphMutationLock mutation_lock(*this);
    auto* n = node(id);
    if (!n) return 1.0f;
    return n->gain;
}

// Drag-add helper.
NodeId add_plugin_node_from_drop(SignalGraph& graph,
                                 const PluginInfo& info,
                                 bool* loaded_out)
{
    // Try the live-load path first. add_plugin_node calls PluginSlot::load,
    // which may return null when the bundle is missing or refuses to load.
    const NodeId id = graph.add_plugin_node(info);
    if (auto* n = graph.node(id); n && n->plugin) {
        if (loaded_out) *loaded_out = true;
        return id;
    }

    // Live-load failed — remove the half-loaded node and create an
    // unresolved placeholder so the graph still carries the user's intent.
    graph.remove_node(id);
    if (loaded_out) *loaded_out = false;
    return graph.add_unresolved_plugin_node(
        info, info.num_inputs, info.num_outputs,
        info.name.empty() ? info.path : info.name);
}

} // namespace pulp::host
