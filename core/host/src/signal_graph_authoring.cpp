// signal_graph_authoring.cpp — control-thread topology authoring and mutation.
//
// Keep node/connection edits under GraphMutationLock and preserve the exact
// invalidate_live_locked_ ordering. No audio-thread execution or publication
// policy is owned by this translation unit.

#include <pulp/format/processor.hpp>
#include <pulp/host/signal_graph.hpp>
#include "signal_graph_ports_internal.hpp"

#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {

bool CustomNodeType::matches_ports(const GraphNode& node) const noexcept {
    if (num_input_ports != node.num_input_ports || num_output_ports != node.num_output_ports ||
        node.input_ports.size() != static_cast<std::size_t>(num_input_ports) ||
        node.output_ports.size() != static_cast<std::size_t>(num_output_ports))
        return false;
    const auto matches = [](const auto& declared, const auto& actual, bool output) {
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto expected_name =
                declared.empty() ? (i == 0 ? std::string("audio")
                                           : std::string(output ? "out" : "in") + std::to_string(i))
                                 : declared[i].name;
            const auto expected_kind = declared.empty() ? PortKind::Audio : declared[i].kind;
            if (actual[i].name != expected_name || actual[i].kind != expected_kind)
                return false;
        }
        return true;
    };
    return matches(input_ports, node.input_ports, false) &&
           matches(output_ports, node.output_ports, true);
}

bool CustomNodeType::is_valid_registration() const noexcept {
    const bool has_plain_callback =
        static_cast<bool>(process) ||
        (static_cast<bool>(create) && static_cast<bool>(process_instance));
    const bool has_transport_callback =
        static_cast<bool>(process_transport) ||
        (static_cast<bool>(create) && static_cast<bool>(process_instance_transport));
    // A latent transport-aware execution path must have a resolvable plain
    // fallback: PDC is prepare-stable and cannot change when a block gains
    // or loses a transport context. Declaring ANY latency callback is the
    // registration-time stand-in for a non-zero fixed latency, since the
    // value itself is not resolvable this early.
    const bool declares_artifact_latency = static_cast<bool>(latency_samples) ||
                                           static_cast<bool>(latency_samples_for_block) ||
                                           static_cast<bool>(output_port_latency_samples);
    if (type_id.empty() || version <= 0 || num_input_ports < 0 || num_output_ports < 0 ||
        (declares_artifact_latency && has_transport_callback && !has_plain_callback)) {
        return false;
    }
    const auto valid_ports = [](const auto& ports, int count) {
        if (ports.empty())
            return true;
        if (ports.size() != static_cast<std::size_t>(count))
            return false;
        for (std::size_t i = 0; i < ports.size(); ++i) {
            if (ports[i].name.empty() || ports[i].name.size() > 128 ||
                static_cast<unsigned>(ports[i].kind) > static_cast<unsigned>(PortKind::Spectral))
                return false;
            for (std::size_t j = 0; j < i; ++j)
                if (ports[j].name == ports[i].name)
                    return false;
        }
        return true;
    };
    if (!valid_ports(input_ports, num_input_ports) || !valid_ports(output_ports, num_output_ports))
        return false;

    // Per-port latency replaces the scalar declaration; accepting both would
    // leave the node's latency ambiguous. The bake layer lowers one scalar
    // latency per node and has no port equaliser, so a per-port node cannot
    // be lowered without silently misaligning its outputs.
    if (output_port_latency_samples &&
        (latency_samples || latency_samples_for_block || lowerable)) {
        return false;
    }

    // A stateful event callback needs a complete instance lifecycle, exactly
    // as the stateful transport and baked-param callbacks do.
    if (process_instance_events && (!create || !destroy))
        return false;

    // The bake layer carries no event plane, so a lowered event-aware node
    // would run its audio silently MIDI-less instead of failing. Refuse the
    // combination at registration, where it is still visible.
    if (lowerable && consumes_events())
        return false;

    // Transport-aware and event-aware execution in one type would need a
    // dispatch precedence the binding does not define. Carrying transport on
    // the event block is what lifts this.
    if (consumes_events() && has_transport_callback)
        return false;

    // Bake-layer obligations live here too, so that every registrar (the
    // direct one, the transactional edit, and the node-adding leaf) applies
    // ONE rule. A descriptor that passes shape but declares an incoherent
    // bake surface would otherwise stay `lowerable`, be accepted by baking,
    // and silently degrade to passthrough.
    const bool declares_baked_params = !baked_params.empty();
    const bool has_baked_param_process = static_cast<bool>(process_instance_baked_param);
    if (declares_baked_params != has_baked_param_process)
        return false;

    if (declares_baked_params) {
        // The callback takes an instance pointer, so a parameter-aware baked
        // type without a complete instance lifecycle can never produce the
        // runnable binding that bake() promises.
        if (!create || !destroy)
            return false;
        for (std::size_t i = 0; i < baked_params.size(); ++i) {
            const auto& param = baked_params[i];
            if (param.id == 0 || !std::isfinite(param.min_value) ||
                !std::isfinite(param.max_value) || !std::isfinite(param.default_value) ||
                param.min_value > param.max_value || param.default_value < param.min_value ||
                param.default_value > param.max_value) {
                return false;
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (baked_params[j].id == param.id)
                    return false;
            }
        }
    }

    if (lowerable) {
        if (create) {
            if (!destroy || (!process_instance && !has_baked_param_process)) {
                return false;
            }
        } else if (!process) {
            return false;
        }
    }
    return true;
}

bool SignalGraph::snapshot_has_isolated_plugins_(const CompiledGraph& snapshot) noexcept {
    for (const auto& [id, plugin] : snapshot.plugins)
        if (plugin && is_isolated_plugin_slot(*plugin))
            return true;
    return false;
}

bool SignalGraph::has_isolated_plugins_locked_() const noexcept {
    for (const auto& node : nodes_)
        if (node.plugin && is_isolated_plugin_slot(*node.plugin))
            return true;
    const auto& live = live_slot_.live();
    return live && snapshot_has_isolated_plugins_(*live);
}

bool SignalGraph::refuse_isolated_mode_locked_(const char* operation) const noexcept {
    if (!has_isolated_plugins_locked_())
        return false;
    runtime::log_error("SignalGraph: {} refused for MacClapStereo64V1", operation);
    return true;
}

void SignalGraph::set_canonical_executor_routing_enabled(bool enabled) noexcept {
    GraphMutationLock lock(*this);
    if (!enabled && refuse_isolated_mode_locked_("legacy executor"))
        return;
    canonical_executor_routing_enabled_.store(enabled, std::memory_order_relaxed);
    detail::advance_execution_selection_generation(execution_selection_generation_);
}

void SignalGraph::set_anticipation_enabled(bool enabled) noexcept {
    GraphMutationLock lock(*this);
    if (enabled && refuse_isolated_mode_locked_("anticipation"))
        return;
    anticipation_enabled_.store(enabled, std::memory_order_relaxed);
    detail::advance_execution_selection_generation(execution_selection_generation_);
}

void SignalGraph::set_parallel_routing_enabled(bool enabled) noexcept {
    GraphMutationLock mutation_lock(*this);
    if (enabled && refuse_isolated_mode_locked_("parallel routing"))
        return;
    // Graph-authored callbacks use one shared, ordered lane.  Parallel
    // dispatch could invoke the consumer before the producer (or concurrently
    // with it), so enabling it after the event edge is bound is refused.
    if (enabled && graph_event_edge_.has_value())
        return;
    parallel_routing_enabled_.store(enabled, std::memory_order_relaxed);
    detail::advance_execution_selection_generation(execution_selection_generation_);
}

namespace {
state::ModulationMixMode modulation_mix_for(AutomationMix mix) {
    switch (mix) {
    case AutomationMix::Replace:
        return state::ModulationMixMode::Replace;
    case AutomationMix::Add:
        return state::ModulationMixMode::Add;
    }
    return state::ModulationMixMode::Add;
}

bool parameter_allows_modulation(const HostParamInfo& p, uint32_t param_id,
                                 state::ParamRate required_rate, bool require_modulatable = false) {
    return p.id == param_id && p.rate == required_rate && p.flags.automatable &&
           (!require_modulatable || p.flags.modulatable) && !p.flags.read_only && !p.flags.stepped;
}

bool has_input_port(const GraphNode& node, PortIndex port) {
    return node.num_input_ports > 0 && port < static_cast<PortIndex>(node.num_input_ports);
}

bool has_output_port(const GraphNode& node, PortIndex port) {
    return node.num_output_ports > 0 && port < static_cast<PortIndex>(node.num_output_ports);
}

// The numeric connection overloads predate typed ports, so they carry only the
// kinds they always carried: audio ports and the single port of a MIDI
// input/output node. A typed port (Control, Event, Param, Spectral, or a
// Custom node's declared MIDI port) must use the named overload so its kind is
// validated instead of being coerced into an audio edge. A port number with no
// handle (a node built without named ports) is a legacy audio port.
bool numeric_overload_carries(const GraphNode& node, PortIndex index, bool output) noexcept {
    const auto& ports = output ? node.output_ports : node.input_ports;
    if (index >= ports.size())
        return true;
    const auto& port = ports[index];
    if (port.kind == PortKind::Audio)
        return true;
    return port.kind == PortKind::Midi &&
           (node.type == NodeType::MidiInput || node.type == NodeType::MidiOutput);
}

void apply_custom_port_descriptors(GraphNode& node,
                                   const std::vector<CustomNodePortDescriptor>& inputs,
                                   const std::vector<CustomNodePortDescriptor>& outputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        node.input_ports[i].name = inputs[i].name;
        node.input_ports[i].kind = inputs[i].kind;
    }
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        node.output_ports[i].name = outputs[i].name;
        node.output_ports[i].kind = outputs[i].kind;
    }
}

} // namespace

namespace detail {

void populate_named_ports(GraphNode& node, std::uint64_t owner, std::uint64_t incarnation) {
    node.input_ports.clear();
    node.output_ports.clear();
    const auto kind = (node.type == NodeType::MidiInput || node.type == NodeType::MidiOutput)
                          ? PortKind::Midi
                          : PortKind::Audio;
    for (int i = 0; i < node.num_input_ports; ++i)
        node.input_ports.push_back({node.id, static_cast<PortIndex>(i), false,
                                    i == 0 ? "audio" : "in" + std::to_string(i), kind, owner,
                                    incarnation});
    for (int i = 0; i < node.num_output_ports; ++i)
        node.output_ports.push_back({node.id, static_cast<PortIndex>(i), true,
                                     i == 0 ? "audio" : "out" + std::to_string(i), kind, owner,
                                     incarnation});
}

} // namespace detail

// All add_*/connect/remove mutators below take graph_mutation_mutex_ for the
// duration of the nodes_/connections_ mutation + invalidate_live_locked_(). The lock is
// safe to hold across invalidate_live_locked_() because that drives only the
// non-blocking Slot::reclaim_if_quiescent() (never the blocking reader-drain), so
// it cannot invert lock order with the Slot reader-pin. See the
// graph_mutation_mutex_ contract in signal_graph.hpp.
NodeId SignalGraph::add_input_node(int channels, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioInput;
    node.name = name;
    node.num_output_ports = channels;
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_output_node(int channels, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::AudioOutput;
    node.name = name;
    node.num_input_ports = channels;
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(const PluginInfo& info) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = info.name;
    node.num_input_ports = info.num_inputs;
    node.num_output_ports = info.num_outputs;
    node.plugin = std::shared_ptr<PluginSlot>(PluginSlot::load(info));
    node.plugin_info = info; // preserve identity even if load failed
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_unresolved_plugin_node(const PluginInfo& info, int num_inputs,
                                               int num_outputs, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Plugin;
    node.name = name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.plugin_info = info;
    node.plugin_info.num_inputs = num_inputs;
    node.plugin_info.num_outputs = num_outputs;
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_plugin_node(std::unique_ptr<PluginSlot> slot, int num_inputs,
                                    int num_outputs, const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
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
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
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
    if (refuse_spectral_mutation_())
        return 0;
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
        detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
        // MIDI is node-scoped; these handles do not consume or renumber audio buses.
        if (descriptor.accepts_midi)
            node.input_ports.push_back({id, 0, false, "midi", PortKind::Midi,
                                        diagnostic_graph_identity_, authoring_generation_ + 1});
        if (descriptor.produces_midi)
            node.output_ports.push_back({id, 0, true, "midi", PortKind::Midi,
                                         diagnostic_graph_identity_, authoring_generation_ + 1});
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

std::shared_ptr<format::ProcessorNodeInstance>
SignalGraph::processor_node_instance(NodeId id) const {
    GraphMutationLock mutation_lock(*this);
    const auto it = processor_nodes_.find(id);
    if (it == processor_nodes_.end() || !it->second)
        return {};
    return it->second->instance;
}

bool SignalGraph::set_processor_audio_input_provider(
    NodeId id, format::AudioInputBusProvider provider) noexcept {
    GraphMutationLock mutation_lock(*this);
    if (!provider)
        return false;
    // ProcessorNode copies the handle before prepare() and reads it from the
    // realtime callback thereafter. Refuse a live graph mutation rather than
    // racing a published snapshot or silently changing a running processor.
    if (refuse_spectral_mutation_() || live_slot_.has_value())
        return false;
    const auto it = processor_nodes_.find(id);
    if (it == processor_nodes_.end() || !it->second || !it->second->instance)
        return false;
    it->second->instance->set_audio_input_provider(provider);
    return true;
}

NodeId SignalGraph::add_gain_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Gain;
    node.name = name;
    node.num_input_ports = 2;
    node.num_output_ports = 2;
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_input_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiInput;
    node.name = name;
    node.num_output_ports = 1;
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

NodeId SignalGraph::add_midi_output_node(const std::string& name) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::MidiOutput;
    node.name = name;
    node.num_input_ports = 1;
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

// add_custom_node overloads only RESOLVE a registered type (read
// custom_node_types_) and delegate to the public add_unresolved_custom_node leaf,
// which takes graph_mutation_mutex_ once for the nodes_ mutation. They do NOT
// take the lock themselves — delegating through another locked public method
// would self-deadlock this non-recursive mutex. Type registration is expected to
// precede topology building (register_custom_node_type holds the same mutex), so
// the custom_node_types_ read here is consistent with that ordering.
NodeId SignalGraph::add_custom_node(std::string_view type_id, const std::string& name) {
    const auto* type = custom_node_type(type_id);
    if (!type)
        return 0;
    return add_custom_node(type_id, type->version, name);
}

NodeId SignalGraph::add_custom_node(std::string_view type_id, int version,
                                    const std::string& name) {
    const auto* type = custom_node_type(type_id, version);
    if (!type)
        return 0;
    return add_unresolved_custom_node(
        type->type_id, type->version, type->num_input_ports, type->num_output_ports,
        name.empty() ? type->default_name : name, type->input_ports, type->output_ports);
}

NodeId SignalGraph::add_control_index_input_node(const std::string& name) {
    const auto* registered = custom_node_type(kControlIndexInputTypeId, kControlIndexInputVersion);
    if (!registered) {
        if (!register_custom_node_type(make_control_index_input_type()))
            return 0;
    } else if (registered->num_input_ports != 0 || registered->num_output_ports != 1 ||
               registered->output_ports.size() != 1 ||
               registered->output_ports[0].name != "index" ||
               registered->output_ports[0].kind != PortKind::Control) {
        return 0;
    }
    return add_custom_node(kControlIndexInputTypeId, kControlIndexInputVersion, name);
}

NodeId SignalGraph::add_unresolved_custom_node(std::string_view type_id, int version,
                                               int num_inputs, int num_outputs,
                                               const std::string& name) {
    return add_unresolved_custom_node(type_id, version, num_inputs, num_outputs, name, {}, {});
}

NodeId
SignalGraph::add_unresolved_custom_node(std::string_view type_id, int version, int num_inputs,
                                        int num_outputs, const std::string& name,
                                        const std::vector<CustomNodePortDescriptor>& input_ports,
                                        const std::vector<CustomNodePortDescriptor>& output_ports) {
    CustomNodeType type;
    type.type_id = std::string(type_id);
    type.version = version;
    type.num_input_ports = num_inputs;
    type.num_output_ports = num_outputs;
    type.default_name = name;
    type.input_ports = input_ports;
    type.output_ports = output_ports;
    if (!type.is_valid_registration())
        return 0;

    // The single locked leaf for the add_custom_node family — serializes the
    // nodes_ push against a concurrent prepare()/mutator.
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return 0;
    if (type_id == kControlIndexInputTypeId) {
        if (version != kControlIndexInputVersion || num_inputs != 0 || num_outputs != 1 ||
            !input_ports.empty() || output_ports.size() != 1 || output_ports[0].name != "index" ||
            output_ports[0].kind != PortKind::Control ||
            std::any_of(nodes_.begin(), nodes_.end(), [](const auto& existing) {
                return existing.type == NodeType::Custom &&
                       existing.custom_type_id == kControlIndexInputTypeId;
            }))
            return 0;
    }
    GraphNode node;
    node.id = next_id_++;
    node.type = NodeType::Custom;
    node.name = name.empty() ? type.type_id : name;
    node.num_input_ports = num_inputs;
    node.num_output_ports = num_outputs;
    node.custom_type_id = std::move(type.type_id);
    node.custom_type_version = version;
    detail::populate_named_ports(node, diagnostic_graph_identity_, authoring_generation_ + 1);
    apply_custom_port_descriptors(node, input_ports, output_ports);
    nodes_.push_back(std::move(node));
    invalidate_live_locked_();
    return nodes_.back().id;
}

bool SignalGraph::remove_node(NodeId id) {
    // Serialize the nodes_ erase against a concurrent compile_()/node() scan on a
    // host thread (see add_gain_node for the lock-ordering rationale).
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (sample_region_for_node_locked_(id) != 0)
        return false;
    auto it =
        std::find_if(nodes_.begin(), nodes_.end(), [id](const GraphNode& n) { return n.id == id; });
    if (it == nodes_.end())
        return false;
    for (std::size_t i = connections_.size(); i-- > 0;) {
        const auto& c = connections_[i];
        if (c.source_node == id || c.dest_node == id) {
            erase_connection_at_locked_(i);
        }
    }
    nodes_.erase(it);
    processor_nodes_.erase(id);
    trigger_producers_.erase(id);
    trigger_envelopes_.erase(id);
    trigger_audio_.erase(id);
    std::erase_if(trigger_edges_,
                  [id](const auto& edge) { return edge.producer == id || edge.consumer == id; });
    if (graph_event_edge_ &&
        (graph_event_edge_->producer == id || graph_event_edge_->consumer == id))
        graph_event_edge_.reset();
    typed_trigger_authoring_.store(!trigger_producers_.empty() || !trigger_envelopes_.empty() ||
                                       !trigger_audio_.empty(),
                                   std::memory_order_release);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect(NodeId source, PortIndex source_port, NodeId dest, PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    // Numeric audio connections cannot identify a frame edge. Frame ports use
    // the named typed overload, which validates identity and producer count.
    if ((src_n->spectral_identity && src_n->type != NodeType::SpectralSynthesis) ||
        (dst_n->spectral_identity && dst_n->type != NodeType::SpectralAnalysis)) {
        spectral_error_.store(SpectralError::InvalidConnection, std::memory_order_relaxed);
        return false;
    }
    if (!has_output_port(*src_n, source_port))
        return false;
    if (!has_input_port(*dst_n, dest_port))
        return false;
    if (!numeric_overload_carries(*src_n, source_port, true) ||
        !numeric_overload_carries(*dst_n, dest_port, false))
        return false;
    if (would_create_cycle(source, dest))
        return false;
    Connection conn{source, source_port, dest, dest_port};
    for (auto& c : connections_)
        if (c == conn)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_midi(NodeId source, NodeId dest) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const auto* src_n = node(source);
    const auto* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    const auto has_control = [](const auto& ports) {
        return std::any_of(ports.begin(), ports.end(),
                           [](const auto& port) { return port.kind == PortKind::Control; });
    };
    if (has_control(src_n->output_ports) || has_control(dst_n->input_ports))
        return false;
    if (would_create_cycle(source, dest))
        return false;
    Connection conn{source, 0, dest, 0, false, true};
    for (auto& c : connections_)
        if (c == conn && c.midi == conn.midi)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_sidechain(NodeId source, PortIndex source_port, NodeId dest,
                                    PortIndex dest_sidechain_port) {
    // Sidechain connections only make sense to Plugin nodes; everything
    // else (Gain, Custom, AudioOutput, ...) has no notion of a sidechain
    // bus. We reject other destinations early so callers fail loudly
    // instead of silently routing into a regular audio port.
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (dst_n->type != NodeType::Plugin)
        return false;
    if (!has_output_port(*src_n, source_port))
        return false;
    if (!has_input_port(*dst_n, dest_sidechain_port))
        return false;
    if (!numeric_overload_carries(*src_n, source_port, true) ||
        !numeric_overload_carries(*dst_n, dest_sidechain_port, false))
        return false;
    if (would_create_cycle(source, dest))
        return false;

    Connection conn{};
    conn.source_node = source;
    conn.source_port = source_port;
    conn.dest_node = dest;
    conn.dest_port = dest_sidechain_port;
    conn.sidechain = true;
    for (auto& c : connections_)
        if (c == conn)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_automation(NodeId src, PortIndex src_audio_port, NodeId dest,
                                     uint32_t dest_param_id, float range_lo, float range_hi,
                                     float smoothing_ms, AutomationMix mix) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(src) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (dst_n->type != NodeType::Plugin || (!dst_n->plugin && !processor_nodes_.contains(dest)))
        return false;
    if (!has_output_port(*src_n, src_audio_port))
        return false;
    if (!numeric_overload_carries(*src_n, src_audio_port, true))
        return false;

    // Reject automation edges that would introduce a cycle. Automation
    // edges contribute to topological order (the source must be processed
    // before the dest), so a back-edge here would make the graph
    // un-orderable. Use the same has_path_locked_ check as connect().
    if (would_create_cycle(src, dest))
        return false;

    // Parameter must exist, be automatable, and not read-only.
    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id)
            continue;
        if (!pi.flags.automatable || pi.flags.read_only)
            return false;
        ok_param = true;
        break;
    }
    if (!ok_param)
        return false;

    // Second Replace edge to same (dest, param) is rejected.
    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.automation && c.dest_node == dest && c.automation_param_id == dest_param_id &&
                c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node = src;
    conn.source_port = src_audio_port;
    conn.dest_node = dest;
    conn.dest_port = 0;
    conn.automation = true;
    conn.automation_param_id = dest_param_id;
    conn.automation_range_lo = range_lo;
    conn.automation_range_hi = range_hi;
    conn.automation_smoothing_ms = std::max(0.0f, smoothing_ms);
    conn.automation_mix = mix;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::connect_audio_rate_modulation(NodeId src, PortIndex src_audio_port, NodeId dest,
                                                uint32_t dest_param_id, float range_lo,
                                                float range_hi, float smoothing_ms,
                                                AutomationMix mix) {
    return connect_audio_rate_modulation_impl_(src, src_audio_port, dest, dest_param_id, range_lo,
                                               range_hi, smoothing_ms, mix, PortKind::Audio,
                                               nullptr);
}

bool SignalGraph::connect_control_modulation(const PortHandle& source, NodeId dest,
                                             uint32_t dest_param_id, float range_lo, float range_hi,
                                             float smoothing_ms, AutomationMix mix) {
    if (!source.output || source.kind != PortKind::Control)
        return false;
    return connect_audio_rate_modulation_impl_(source.node, source.index, dest, dest_param_id,
                                               range_lo, range_hi, smoothing_ms, mix,
                                               PortKind::Control, &source);
}

bool SignalGraph::connect_audio_rate_modulation_impl_(NodeId src, PortIndex src_audio_port,
                                                      NodeId dest, uint32_t dest_param_id,
                                                      float range_lo, float range_hi,
                                                      float smoothing_ms, AutomationMix mix,
                                                      PortKind expected_kind,
                                                      const PortHandle* source_handle) {
    // Holds graph_mutation_mutex_ across audio_rate_modulation_lane() below, which
    // is a lock-free helper that assumes the caller holds it (it scans nodes_).
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(src) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(src);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (dst_n->type != NodeType::Plugin || (!dst_n->plugin && !processor_nodes_.contains(dest)))
        return false;
    if (!has_output_port(*src_n, src_audio_port))
        return false;
    if (expected_kind == PortKind::Control) {
        if (src_audio_port >= src_n->output_ports.size())
            return false;
        const auto& port = src_n->output_ports[src_audio_port];
        if (port.kind != PortKind::Control ||
            (source_handle &&
             (port.name != source_handle->name || port.owner != source_handle->owner ||
              port.incarnation != source_handle->incarnation)))
            return false;
    } else if (!numeric_overload_carries(*src_n, src_audio_port, true)) {
        return false;
    }
    if (would_create_cycle(src, dest))
        return false;

    bool ok_param = false;
    for (const auto& pi : cached_or_live_params_locked_(*dst_n)) {
        if (pi.id != dest_param_id)
            continue;
        if (!parameter_allows_modulation(pi, dest_param_id, state::ParamRate::AudioRate, true)) {
            return false;
        }
        ok_param = true;
        break;
    }
    if (!ok_param)
        return false;

    if (mix == AutomationMix::Replace) {
        for (const auto& c : connections_) {
            if (c.audio_rate_modulation && c.dest_node == dest &&
                c.automation_param_id == dest_param_id &&
                c.automation_mix == AutomationMix::Replace) {
                return false;
            }
        }
    }

    Connection conn{};
    conn.source_node = src;
    conn.source_port = src_audio_port;
    conn.dest_node = dest;
    conn.dest_port = 0;
    conn.audio_rate_modulation = true;
    conn.control_signal = expected_kind == PortKind::Control;
    conn.automation_param_id = dest_param_id;
    conn.automation_range_lo = range_lo;
    conn.automation_range_hi = range_hi;
    conn.automation_smoothing_ms = std::max(0.0f, smoothing_ms);
    conn.automation_mix = mix;
    state::ModulationLane lane;
    // Mutex already held: call the lock-free core directly (the public
    // audio_rate_modulation_lane would re-lock and self-deadlock).
    if (!audio_rate_modulation_lane_locked_(conn, lane))
        return false;
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
        if (pi.id != connection.automation_param_id)
            continue;

        lane = state::ModulationLane{
            .source =
                {
                    .id = static_cast<state::ModulationSourceId>(connection.source_node),
                    .scope = state::ModulationScope::GraphNode,
                    .rate = state::ModulationRate::Audio,
                },
            .target =
                {
                    .param_id = pi.id,
                    .scope = state::ModulationScope::GraphNode,
                    .param_rate = pi.rate,
                    .modulatable =
                        pi.flags.modulatable && pi.flags.automatable && !pi.flags.stepped,
                    .writable = !pi.flags.read_only,
                },
            .mix = modulation_mix_for(connection.automation_mix),
            .depth = std::abs(connection.automation_range_hi - connection.automation_range_lo),
        };
        return state::validate_modulation_lane(lane).accepted;
    }

    return false;
}

bool SignalGraph::connect_feedback(NodeId source, PortIndex source_port, NodeId dest,
                                   PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    const GraphNode* src_n = node(source);
    const GraphNode* dst_n = node(dest);
    if (!src_n || !dst_n)
        return false;
    if (!has_output_port(*src_n, source_port))
        return false;
    if (!has_input_port(*dst_n, dest_port))
        return false;
    if (!numeric_overload_carries(*src_n, source_port, true) ||
        !numeric_overload_carries(*dst_n, dest_port, false))
        return false;
    Connection conn{source, source_port, dest, dest_port, true};
    for (auto& c : connections_)
        if (c == conn)
            return false;
    append_connection_locked_(conn);
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::disconnect(NodeId source, PortIndex source_port, NodeId dest,
                             PortIndex dest_port) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
    if (prepared_edit_origin_ == nullptr &&
        (sample_region_for_node_locked_(source) != 0 || sample_region_for_node_locked_(dest) != 0))
        return false;
    Connection target{source, source_port, dest, dest_port};
    if (const auto* n = node(source))
        target.spectral = n->spectral_identity && n->type != NodeType::SpectralSynthesis;
    auto it = std::find(connections_.begin(), connections_.end(), target);
    if (it == connections_.end())
        return false;
    erase_connection_at_locked_(static_cast<std::size_t>(std::distance(connections_.begin(), it)));
    if (graph_event_edge_ &&
        !has_path_locked_(graph_event_edge_->producer, graph_event_edge_->consumer))
        graph_event_edge_.reset();
    invalidate_live_locked_();
    return true;
}

bool SignalGraph::disconnect_modulation(NodeId source, PortIndex source_port, NodeId dest,
                                        uint32_t dest_param_id, bool audio_rate) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return false;
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
    if (graph_event_edge_ &&
        !has_path_locked_(graph_event_edge_->producer, graph_event_edge_->consumer))
        graph_event_edge_.reset();
    invalidate_live_locked_();
    return true;
}

// Does not lock: scans nodes_; the caller MUST hold graph_mutation_mutex_
// (every internal mutator/reader below does). Public direct callers
// (e.g. nodes()/node() external users) own their serialization per the accessor
// contract documented on graph_mutation_mutex_.
const GraphNode* SignalGraph::node(NodeId id) const {
    for (auto& n : nodes_)
        if (n.id == id)
            return &n;
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
    connection_identities_.erase(connection_identities_.begin() +
                                 static_cast<std::ptrdiff_t>(index));
}

bool SignalGraph::has_path_locked_(NodeId from, NodeId to) const {
    std::unordered_set<NodeId> visited;
    std::queue<NodeId> queue;
    queue.push(from);
    while (!queue.empty()) {
        auto current = queue.front();
        queue.pop();
        if (current == to)
            return true;
        if (visited.count(current))
            continue;
        visited.insert(current);
        for (auto& c : connections_) {
            if (c.feedback)
                continue;
            if (c.source_node == current)
                queue.push(c.dest_node);
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
    std::unordered_map<NodeId, std::size_t> authoring_index;
    authoring_index.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& n = nodes[index];
        in_degree[n.id] = 0;
        authoring_index[n.id] = index;
    }
    for (const auto& c : connections) {
        if (c.feedback)
            continue;
        in_degree[c.dest_node]++;
    }
    // A graph can have multiple ready nodes at every fan-out/fan-in boundary.
    // Do not let unordered_map iteration or connection insertion order choose
    // the runtime plan. Authoring position is the semantic tie-break for
    // independent nodes; NodeId is an identity token and may outlive a node's
    // position after removals or future ID allocation changes.
    using ReadyNode = std::pair<std::size_t, NodeId>;
    std::priority_queue<ReadyNode, std::vector<ReadyNode>, std::greater<ReadyNode>> queue;
    for (const auto& [id, deg] : in_degree)
        if (deg == 0)
            queue.push({authoring_index.at(id), id});
    std::vector<NodeId> order;
    while (!queue.empty()) {
        const auto current = queue.top().second;
        queue.pop();
        order.push_back(current);
        for (const auto& c : connections) {
            if (c.feedback)
                continue;
            // Automation edges DO contribute to topological order — the
            // source must be processed before the dest so its output
            // buffer is valid when we sample it for param events.
            if (c.source_node == current) {
                if (--in_degree[c.dest_node] == 0)
                    queue.push({authoring_index.at(c.dest_node), c.dest_node});
            }
        }
    }
    return order;
}
} // namespace

std::vector<NodeId> SignalGraph::processing_order() const {
    return processing_order_for(nodes_, connections_);
}

} // namespace pulp::host

namespace pulp::host {

PortHandle GraphNode::in(std::string_view port) const {
    for (const auto& p : input_ports)
        if (p.name == port)
            return p;
    return PortHandle{id, 0, false, std::string(port), PortKind::Audio};
}
PortHandle GraphNode::out(std::string_view port) const {
    for (const auto& p : output_ports)
        if (p.name == port)
            return p;
    return PortHandle{id, 0, true, std::string(port), PortKind::Audio};
}

GraphResult SignalGraph::connect(const PortHandle& source, const PortHandle& dest) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return {GraphResult::Code::UnknownPort, 0, 0, "spectral mutation refused", {}};
    const GraphNode* src = node(source.node);
    const GraphNode* dst = node(dest.node);
    if (!src || !dst)
        return {GraphResult::Code::UnknownNode, !src ? source.node : dest.node, 0, "unknown node",
                "use a node from this graph"};
    if (!source.output)
        return {GraphResult::Code::UnknownPort, source.node, source.index,
                "source handle is not an output port", "use out(\"audio\") or an available output"};
    if (dest.output)
        return {GraphResult::Code::UnknownPort, dest.node, dest.index,
                "destination handle is not an input port",
                "use in(\"audio\") or an available input"};
    if (prepared_edit_origin_ == nullptr && (sample_region_for_node_locked_(source.node) != 0 ||
                                             sample_region_for_node_locked_(dest.node) != 0))
        return {GraphResult::Code::UnknownPort, dest.node, dest.index,
                "sample-region nodes cannot be mutated directly", "use a prepared topology edit"};
    auto find = [](const std::vector<PortHandle>& ports, const PortHandle& h) -> const PortHandle* {
        for (const auto& p : ports)
            if (p.name == h.name)
                return &p;
        return nullptr;
    };
    if (source.kind != dest.kind)
        return {GraphResult::Code::KindMismatch, dest.node, dest.index,
                "port kind mismatch: '" + source.name + "' to '" + dest.name + "'",
                "connect ports with the same kind"};
    const auto* sp = find(src->output_ports, source);
    const auto* dp = find(dst->input_ports, dest);
    if (!sp)
        return {GraphResult::Code::UnknownPort, source.node, source.index,
                "unknown output port '" + source.name + "'",
                "use out(\"audio\") or an available output"};
    if (!dp)
        return {GraphResult::Code::UnknownPort, dest.node, dest.index,
                "unknown input port '" + dest.name + "'",
                "use in(\"audio\") or an available input"};
    if (sp->kind != dp->kind || source.kind != sp->kind || dest.kind != dp->kind)
        return {GraphResult::Code::KindMismatch, dest.node, dp->index,
                "port kind mismatch: '" + source.name + "' to '" + dest.name + "'",
                "connect ports with the same kind"};
    if ((sp->owner && (sp->owner != source.owner || sp->incarnation != source.incarnation)) ||
        (dp->owner && (dp->owner != dest.owner || dp->incarnation != dest.incarnation)))
        return {GraphResult::Code::UnknownPort, dest.node, dp->index,
                "stale or foreign port handle", "obtain this graph's current port"};
    if (dst->type == NodeType::SpectralAssetMorph) {
        if (src->type != NodeType::AudioInput || dp->name != "morph" || sp->kind != PortKind::Audio)
            return {GraphResult::Code::KindMismatch,
                    dest.node,
                    dp->index,
                    "asset morph requires a direct host AudioInput",
                    {}};
        for (const auto& edge : connections_)
            if (edge.dest_node == dest.node)
                return {GraphResult::Code::Duplicate,
                        dest.node,
                        dp->index,
                        "asset morph requires one control producer",
                        {}};
    }
    if (sp->kind == PortKind::Spectral) {
        if (!src->spectral_identity || !dst->spectral_identity ||
            src->spectral_identity != dst->spectral_identity)
            return {GraphResult::Code::KindMismatch,
                    dest.node,
                    dp->index,
                    "spectral identity mismatch",
                    {}};
        for (const auto& edge : connections_)
            if (edge.spectral && edge.dest_node == dest.node)
                return {GraphResult::Code::Duplicate,
                        dest.node,
                        dp->index,
                        "spectral joins are unsupported",
                        {}};
    }
    if (would_create_cycle(source.node, dest.node))
        return {GraphResult::Code::IllegalCycle, dest.node, dp->index,
                "illegal instantaneous cycle", "insert a feedback delay"};
    Connection c{source.node, sp->index, dest.node, dp->index};
    c.spectral = sp->kind == PortKind::Spectral;
    c.control_signal = sp->kind == PortKind::Control;
    c.midi = sp->kind == PortKind::Midi;
    for (const auto& existing : connections_)
        if (existing == c && existing.midi == c.midi)
            return {GraphResult::Code::Duplicate, dest.node, dp->index, "duplicate connection",
                    "remove the existing connection"};
    append_connection_locked_(c);
    invalidate_live_locked_();
    return {};
}
} // namespace pulp::host
