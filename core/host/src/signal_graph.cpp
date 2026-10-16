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
#include <functional>
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

SignalGraph::ProcessorNodeLifetime::ProcessorNodeLifetime(
    std::shared_ptr<format::ProcessorNodeInstance> value) noexcept
    : instance(std::move(value)) {}

SignalGraph::ProcessorNodeLifetime::~ProcessorNodeLifetime() {
    if (instance)
        (void)instance->release();
}

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
    return prepared_max_block_size_.load(std::memory_order_relaxed);
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
    auto read_guard = live_slot_.read();
    const auto* live = read_guard.get();
    if (live == nullptr || block_size <= 0)
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
