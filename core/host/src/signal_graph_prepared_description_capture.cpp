// SPDX-License-Identifier: MIT
//
// Prepared-description capture for a compiled prepared-topology candidate.
// See signal_graph_prepared_description_capture.hpp for the contract.

#include "signal_graph_prepared_description_capture.hpp"

#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/prepared_description.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::host::detail {

const char* prepared_kind(NodeType type) noexcept {
    switch (type) {
    case NodeType::AudioInput:
        return "AudioInput";
    case NodeType::AudioOutput:
        return "AudioOutput";
    case NodeType::Gain:
        return "Gain";
    default:
        return nullptr;
    }
}

std::shared_ptr<PreparedDescriptionData>
capture_prepared_description(const SignalGraph& candidate, const PreparedDescriptionData& seed,
                             const format::GraphRuntimeSnapshot& serial_snapshot,
                             const format::GraphRuntimeBufferPool& serial_pool, double sample_rate,
                             int max_block_size, std::uint64_t execution_selection_generation,
                             std::uint64_t execution_generation, std::uint64_t owner_generation) {
    auto capture = std::make_shared<PreparedDescriptionData>(seed);
    capture->sample_rate = sample_rate;
    capture->max_block_frames = static_cast<std::uint64_t>(max_block_size);
    capture->canonical_executor_routing_enabled = candidate.canonical_executor_routing_enabled();
    capture->parallel_routing_enabled = candidate.parallel_routing_enabled();
    capture->anticipation_enabled = candidate.anticipation_enabled();
    capture->execution_selection_generation = execution_selection_generation;
    capture->pdc_execution_domain = "dynamic";
    capture->executor_contract = "pulp.graph.canonical-executor.f32.v1";
    capture->execution_mode = "serial";
    capture->tie_break_contract = "ready-node-id-ascending.v1";
    capture->execution_fallback_policy = "internal_copy";
    const auto limits = candidate.limits();
    capture->host_max_nodes = static_cast<std::uint64_t>(limits.max_nodes);
    capture->host_max_connections = static_cast<std::uint64_t>(limits.max_connections);
    capture->host_max_ports = static_cast<std::uint64_t>(limits.max_ports);
    capture->host_max_block_frames =
        limits.max_block_size > 0 ? static_cast<std::uint64_t>(limits.max_block_size) : 0;
    capture->host_max_estimated_work_units =
        static_cast<std::uint64_t>(limits.max_estimated_work_units);
    // The routed planner is compiled with this closed, explicit limit set.
    // Capture those actual limits instead of copying GraphLimits, whose
    // defaults are intentionally broader than the executor contract.
    const graph::GraphRuntimeLimits runtime_limits{};
    capture->runtime_max_nodes = runtime_limits.max_nodes;
    capture->runtime_max_connections = runtime_limits.max_connections;
    capture->runtime_max_ports_per_node = runtime_limits.max_ports_per_node;
    capture->runtime_max_ports = runtime_limits.max_total_ports;
    // The routed pool is the runtime's actual frame capacity for this
    // prepared serial path. It is value-owned here rather than inferred
    // from the host request or a default limit struct.
    capture->runtime_max_block_frames = serial_pool.max_frames();
    const auto effective_bound = [](std::uint64_t profile, std::uint64_t host,
                                    std::uint64_t runtime) {
        std::uint64_t result = profile;
        if (host != 0)
            result = std::min(result, host);
        if (runtime != 0)
            result = std::min(result, runtime);
        return result;
    };
    capture->effective_max_nodes = effective_bound(
        capture->profile_max_nodes, capture->host_max_nodes, capture->runtime_max_nodes);
    capture->effective_max_connections =
        effective_bound(capture->profile_max_connections, capture->host_max_connections,
                        capture->runtime_max_connections);
    capture->effective_max_ports_per_node = effective_bound(capture->profile_max_ports_per_node, 0,
                                                            capture->runtime_max_ports_per_node);
    capture->effective_max_ports = effective_bound(
        capture->profile_max_ports, capture->host_max_ports, capture->runtime_max_ports);
    capture->effective_max_block_frames =
        effective_bound(capture->profile_max_block_frames, capture->host_max_block_frames,
                        capture->runtime_max_block_frames);
    if (capture->nodes.size() > capture->effective_max_nodes ||
        capture->edges.size() > capture->effective_max_connections ||
        std::any_of(capture->nodes.begin(), capture->nodes.end(),
                    [&](const auto& node) {
                        return std::max(node.input_ports, node.output_ports) >
                               capture->effective_max_ports_per_node;
                    }) ||
        static_cast<std::uint64_t>(max_block_size) > capture->effective_max_block_frames)
        return nullptr;
    capture->snapshot_id = serial_snapshot.id();
    // Preserve the pool's actual binding token. It is separate from the
    // snapshot identity so an export cannot silently bless an unbound or
    // mismatched pool that merely has matching capacities.
    capture->pool_snapshot_id = serial_pool.bound_snapshot_id();
    capture->pool_binding_identity = reinterpret_cast<std::uintptr_t>(&serial_pool);
    capture->execution_generation = execution_generation;
    capture->owner_generation = owner_generation;
    const auto& plan = serial_snapshot.plan();
    if (!serial_snapshot.valid() || plan.nodes.size() != capture->nodes.size() ||
        plan.connections.size() != capture->edges.size() ||
        plan.nodes.size() > capture->effective_max_nodes ||
        plan.connections.size() > capture->effective_max_connections ||
        plan.nodes.size() > std::numeric_limits<std::uint32_t>::max() ||
        plan.connections.size() > std::numeric_limits<std::uint32_t>::max())
        return nullptr;
    capture->prepared_plan_node_count = plan.nodes.size();
    capture->prepared_plan_connection_count = plan.connections.size();
    capture->serial_pool_bound = serial_pool.bound_to(serial_snapshot);
    const auto& assignment = serial_snapshot.buffer_assignment();
    if (assignment.nodes.size() != plan.nodes.size() ||
        assignment.connection_delay_samples.size() != plan.connections.size() ||
        assignment.reads_in_place.size() != plan.connections.size())
        return nullptr;
    capture->buffer_slot_count = assignment.slot_count;
    capture->pool_slot_count = serial_pool.slot_count();
    capture->pool_max_frames = serial_pool.max_frames();
    capture->total_latency_samples = assignment.total_latency_samples;
    capture->connection_delay_samples = assignment.connection_delay_samples;
    capture->reads_in_place = assignment.reads_in_place;
    if (!capture->serial_pool_bound || capture->pool_snapshot_id != capture->snapshot_id ||
        capture->pool_binding_identity == 0 ||
        capture->pool_slot_count != capture->buffer_slot_count ||
        capture->pool_max_frames < capture->max_block_frames)
        return nullptr;
    capture->host_input_alias_eligible = false;
    capture->host_input_alias_mode = "conditional_direct_host_input";
    capture->host_input_alias_fallback = "internal_copy";
    capture->host_input_alias_requires_full_width = true;
    capture->host_input_alias_requires_disjoint = true;
    capture->host_input_alias_mappings.clear();
    const auto mode_name = [](format::GraphRuntimeSnapshot::InputPortSource::Mode mode) {
        switch (mode) {
        case format::GraphRuntimeSnapshot::InputPortSource::Mode::Zero:
            return "zero";
        case format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace:
            return "in_place";
        case format::GraphRuntimeSnapshot::InputPortSource::Mode::Copy:
            return "copy";
        case format::GraphRuntimeSnapshot::InputPortSource::Mode::Sum:
            return "sum";
        }
        return "unknown";
    };
    const auto bindings = serial_snapshot.bindings();
    if (bindings.size() != plan.nodes.size())
        return nullptr;
    std::vector<std::size_t> authored_positions(plan.nodes.size());
    std::vector<std::size_t> processing_positions(plan.nodes.size(),
                                                  std::numeric_limits<std::size_t>::max());
    for (std::size_t position = 0; position < plan.processing_order_indices.size(); ++position) {
        const auto dense = plan.processing_order_indices[position];
        if (dense >= plan.nodes.size() ||
            processing_positions[dense] != std::numeric_limits<std::size_t>::max())
            return nullptr;
        processing_positions[dense] = position;
    }
    if (plan.processing_order_indices.size() != plan.nodes.size())
        return nullptr;

    // The portable binding was checked against the authored graph before
    // preparation. Recheck the compiled plan's identity and every edge
    // against that same graph before exporting: a planner or adapter
    // substitution must not be able to preserve counts while changing a
    // kind, port shape, gain binding, or endpoint tuple.
    const auto expected_runtime_kind = [](NodeType type) {
        switch (type) {
        case NodeType::AudioInput:
            return graph::GraphRuntimeNodeKind::AudioInput;
        case NodeType::AudioOutput:
            return graph::GraphRuntimeNodeKind::AudioOutput;
        case NodeType::Gain:
            return graph::GraphRuntimeNodeKind::Processor;
        default:
            return graph::GraphRuntimeNodeKind::Utility;
        }
    };
    for (const auto& pnode : plan.nodes) {
        const auto authored =
            std::find_if(candidate.nodes().begin(), candidate.nodes().end(),
                         [&](const GraphNode& node) { return node.id == pnode.id; });
        if (authored == candidate.nodes().end() ||
            pnode.kind != expected_runtime_kind(authored->type) ||
            pnode.input_ports != static_cast<std::uint32_t>(authored->num_input_ports) ||
            pnode.output_ports != static_cast<std::uint32_t>(authored->num_output_ports))
            return nullptr;
        const auto authored_position =
            static_cast<std::size_t>(std::distance(candidate.nodes().begin(), authored));
        if (authored_position >= capture->nodes.size())
            return nullptr;
        const auto& bound = capture->nodes[authored_position];
        if (bound.kind != prepared_kind(authored->type) || bound.input_ports != pnode.input_ports ||
            bound.output_ports != pnode.output_ports ||
            (authored->type == NodeType::Gain &&
             bound.gain_bits != std::bit_cast<std::uint32_t>(authored->gain)))
            return nullptr;
    }
    std::unordered_set<std::string> plan_edge_keys;
    plan_edge_keys.reserve(plan.connections.size());
    for (const auto& pconnection : plan.connections) {
        if (pconnection.source_index >= plan.nodes.size() ||
            pconnection.dest_index >= plan.nodes.size() || !graph::carries_audio(pconnection) ||
            pconnection.feedback || pconnection.sidechain)
            return nullptr;
        const auto& source = plan.nodes[pconnection.source_index];
        const auto& destination = plan.nodes[pconnection.dest_index];
        const auto key = std::to_string(source.id) + ":" + std::to_string(pconnection.source_port) +
                         ":" + std::to_string(destination.id) + ":" +
                         std::to_string(pconnection.dest_port);
        if (!plan_edge_keys.insert(key).second ||
            std::none_of(candidate.connections().begin(), candidate.connections().end(),
                         [&](const Connection& connection) {
                             return connection.source_node == source.id &&
                                    connection.source_port == pconnection.source_port &&
                                    connection.dest_node == destination.id &&
                                    connection.dest_port == pconnection.dest_port &&
                                    !connection.feedback && !connection.midi &&
                                    !connection.automation && !connection.audio_rate_modulation &&
                                    !connection.sidechain && !connection.spectral &&
                                    !connection.control_signal;
                         }))
            return nullptr;
    }
    if (plan_edge_keys.size() != candidate.connections().size())
        return nullptr;
    capture->nodes.resize(plan.nodes.size());
    for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
        const auto authored =
            std::find_if(candidate.nodes().begin(), candidate.nodes().end(),
                         [&](const GraphNode& node) { return node.id == plan.nodes[dense].id; });
        if (authored == candidate.nodes().end() || dense >= assignment.nodes.size())
            return nullptr;
        const auto position = static_cast<std::size_t>(authored - candidate.nodes().begin());
        if (position >= capture->nodes.size())
            return nullptr;
        authored_positions[dense] = position;
        auto& node = capture->nodes[position];
        node.input_ports = plan.nodes[dense].input_ports;
        node.output_ports = plan.nodes[dense].output_ports;
        node.latency_samples = plan.nodes[dense].latency_samples;
        node.persistent_output = plan.nodes[dense].persistent_output;
        node.reads_inputs_only = bindings[dense].reads_inputs_only;
        node.transport_sensitive = authored->transport_sensitive;
        node.tail_samples = 0;
        if (node.input_ports > runtime_limits.max_ports_per_node ||
            node.output_ports > runtime_limits.max_ports_per_node)
            return nullptr;
    }
    std::uint64_t total_ports = 0;
    for (const auto& node : plan.nodes) {
        if (node.input_ports > std::numeric_limits<std::uint64_t>::max() - total_ports)
            return nullptr;
        total_ports += node.input_ports;
        if (node.output_ports > std::numeric_limits<std::uint64_t>::max() - total_ports)
            return nullptr;
        total_ports += node.output_ports;
    }
    if (total_ports > capture->effective_max_ports)
        return nullptr;

    capture->input_modes.clear();
    capture->schedule.clear();
    for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
        const auto& key = capture->nodes[authored_positions[dense]].authored_key;
        const auto sources = serial_snapshot.input_port_sources(dense);
        if (sources.size() != plan.nodes[dense].input_ports)
            return nullptr;
        for (std::uint32_t port = 0; port < sources.size(); ++port) {
            if (sources[port].mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::Sum)
                return nullptr;
            if (port >= capture->channels.size())
                return nullptr;
            capture->input_modes.push_back(key + ":port=audio:channel=" + capture->channels[port] +
                                           ",mode=" + mode_name(sources[port].mode));
        }
    }
    for (const auto dense_index : plan.processing_order_indices)
        capture->schedule.push_back(capture->nodes[authored_positions[dense_index]].authored_key);

    // Copy the actual node binding and edge lowering facts. The initial
    // authored record is deliberately not trusted as a plan substitute.
    for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
        const auto& pnode = plan.nodes[dense];
        auto& node = capture->nodes[authored_positions[dense]];
        node.input_ports = pnode.input_ports;
        node.output_ports = pnode.output_ports;
        node.latency_samples = pnode.latency_samples;
        node.persistent_output = pnode.persistent_output;
        node.reads_inputs_only = bindings[dense].reads_inputs_only;
    }
    const auto final_mode = [&](std::uint32_t dense, std::uint32_t port) {
        const auto sources = serial_snapshot.input_port_sources(dense);
        return sources.size() > port ? mode_name(sources[port].mode) : "unknown";
    };
    std::vector<PreparedDescriptionEdge> actual_edges;
    actual_edges.reserve(plan.connections.size());
    for (std::uint32_t index = 0; index < plan.connections.size(); ++index) {
        const auto& connection = plan.connections[index];
        if (connection.source_index >= plan.nodes.size() ||
            connection.dest_index >= plan.nodes.size() ||
            index >= assignment.connection_delay_samples.size() ||
            index >= assignment.reads_in_place.size())
            return nullptr;
        const auto& source_key =
            capture->nodes[authored_positions[connection.source_index]].authored_key;
        const auto& destination_key =
            capture->nodes[authored_positions[connection.dest_index]].authored_key;
        std::size_t gather = 0;
        const auto& destination = plan.nodes[connection.dest_index];
        bool found = false;
        for (std::uint32_t offset = 0; offset < destination.inbound_connection_count; ++offset) {
            const auto at = destination.first_inbound_connection + offset;
            if (at < plan.inbound_connection_indices.size() &&
                plan.inbound_connection_indices[at] == index) {
                gather = offset;
                found = true;
                break;
            }
        }
        if (!found)
            return nullptr;
        const std::string_view mode = final_mode(connection.dest_index, connection.dest_port);
        if (mode == "sum" || mode == "unknown" || !graph::carries_audio(connection) ||
            connection.feedback || assignment.connection_delay_samples[index] != 0)
            return nullptr;
        if (connection.source_port >= capture->channels.size() ||
            connection.dest_port >= capture->channels.size())
            return nullptr;
        actual_edges.push_back(
            {source_key + ":port=audio:channel=" + capture->channels[connection.source_port],
             destination_key + ":port=audio:channel=" + capture->channels[connection.dest_port],
             connection.source_port, connection.dest_port, std::string(mode),
             static_cast<std::uint32_t>(gather), assignment.connection_delay_samples[index],
             connection.feedback, assignment.reads_in_place[index] != 0, std::string(mode)});
    }
    capture->edges = std::move(actual_edges);

    // Recompute the host-input policy using the same facts consumed by
    // GraphRuntimeBufferPool::bind(), including read-only bindings and
    // resolved in-place input modes. The policy is conditional; it never
    // exports a callback-time pointer.
    std::uint32_t input_dense = 0;
    std::size_t input_count = 0;
    for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
        if (plan.nodes[dense].kind == graph::GraphRuntimeNodeKind::AudioInput) {
            input_dense = dense;
            ++input_count;
        }
    }
    bool aliasable = input_count == 1;
    for (const auto& connection : plan.connections) {
        if (!aliasable || connection.source_index != input_dense)
            continue;
        const auto ports = serial_snapshot.input_port_sources(connection.dest_index);
        const auto& destination = plan.nodes[connection.dest_index];
        const bool read_only = destination.kind == graph::GraphRuntimeNodeKind::AudioOutput ||
                               bindings[connection.dest_index].reads_inputs_only;
        if (!graph::carries_audio(connection) || connection.feedback || !read_only ||
            connection.dest_port >= ports.size() ||
            ports[connection.dest_port].mode !=
                format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace)
            aliasable = false;
    }
    capture->host_input_alias_eligible = aliasable && input_count == 1;
    if (capture->host_input_alias_eligible &&
        plan.nodes[input_dense].output_ports != capture->channels.size())
        capture->host_input_alias_eligible = false;
    if (capture->host_input_alias_eligible) {
        capture->host_input_alias_source =
            capture->nodes[authored_positions[input_dense]].authored_key + ":audio:out";
        capture->host_input_alias_view =
            capture->nodes[authored_positions[input_dense]].authored_key + ":audio:out";
        for (const auto& channel : capture->channels)
            capture->host_input_alias_mappings.push_back(
                capture->host_input_alias_source + ":channel=" + channel + "->" +
                capture->host_input_alias_view + ":channel=" + channel);
    }

    // Capture typed values before touching the legacy delimiter records.
    // A view refers to its producer's value; it is never a new write.
    capture->resolved_node_bindings.clear();
    capture->resolved_edge_bindings.clear();
    capture->resolved_buffer_classes.clear();
    capture->resolved_buffer_values.clear();
    capture->resolved_alias_endpoint.reset();
    const auto endpoint = [&](std::uint32_t dense, bool output) {
        return ResolvedEndpoint{capture->nodes[authored_positions[dense]].authored_key, output};
    };
    for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
        const auto& binding = bindings[dense];
        if (binding.audio_rate_modulation_delivery !=
            format::AudioRateModulationDelivery::LegacyParameterEvents)
            return nullptr;
        capture->resolved_node_bindings.push_back({endpoint(dense, true).node, binding.required,
                                                   binding.preserve_parameter_events, "none"});
    }
    for (const auto& connection : plan.connections)
        capture->resolved_edge_bindings.push_back(
            {endpoint(connection.source_index, true), endpoint(connection.dest_index, false),
             capture->channels[connection.source_port], capture->channels[connection.dest_port]});
    if (capture->host_input_alias_eligible)
        capture->resolved_alias_endpoint = endpoint(input_dense, true);
    const auto add_typed_class = [&](std::uint32_t id, std::string kind) {
        auto& classes = capture->resolved_buffer_classes;
        if (std::none_of(classes.begin(), classes.end(),
                         [&](const auto& value) { return value.id == id; }))
            classes.push_back(
                {id, std::move(kind), capture->max_block_frames, "f32", "prepare_zero", {}});
    };
    std::vector<std::vector<std::size_t>> produced(plan.nodes.size());
    for (const auto dense : plan.processing_order_indices) {
        const auto& pnode = plan.nodes[dense];
        const auto& slots = assignment.nodes[dense];
        const auto position = static_cast<std::uint32_t>(processing_positions[dense]);
        const auto sources = serial_snapshot.input_port_sources(dense);
        for (std::uint32_t port = 0; port < pnode.input_ports; ++port) {
            const auto mode = sources[port].mode;
            std::optional<std::size_t> incoming;
            for (const auto& edge : plan.connections)
                if (edge.dest_index == dense && edge.dest_port == port) {
                    if (incoming || edge.source_index >= produced.size() ||
                        edge.source_port >= produced[edge.source_index].size())
                        return nullptr;
                    incoming = produced[edge.source_index][edge.source_port];
                }
            std::size_t value_index;
            if (mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace) {
                if (!incoming)
                    return nullptr;
                value_index = *incoming;
                auto& value = capture->resolved_buffer_values[value_index];
                if (value.class_id != sources[port].slot)
                    return nullptr;
                value.accesses.push_back({endpoint(dense, false), capture->channels[port],
                                          "input_view", "execute", port, "in_place_read"});
                value.last_read_phase = "execute";
                value.lifetime_end = std::max(value.lifetime_end, position + 1);
            } else {
                if (incoming) {
                    auto& value = capture->resolved_buffer_values[*incoming];
                    value.accesses.push_back({endpoint(dense, false), capture->channels[port],
                                              "input_view", "gather", port, "read"});
                    value.last_read_phase = "gather";
                    value.lifetime_end = std::max(value.lifetime_end, position + 1);
                }
                const auto slot = slots.input_base + port;
                add_typed_class(slot, "scratch");
                value_index = capture->resolved_buffer_values.size();
                capture->resolved_buffer_values.push_back(
                    {endpoint(dense, false),
                     capture->channels[port],
                     "gather",
                     port,
                     slot,
                     {{endpoint(dense, false), capture->channels[port], "input_scratch", "gather",
                       port, incoming ? "copy" : "zero"},
                      {endpoint(dense, false), capture->channels[port], "input_scratch", "execute",
                       port, "read"}},
                     position,
                     position + 1,
                     "execute"});
            }
            if (pnode.kind == graph::GraphRuntimeNodeKind::AudioOutput) {
                auto& value = capture->resolved_buffer_values[value_index];
                value.accesses.push_back({endpoint(dense, false), capture->channels[port],
                                          "external_output", "output_copy", port, "read"});
                value.last_read_phase = "output_copy";
                const auto external = assignment.slot_count + port;
                add_typed_class(external, "external_output");
                auto& klass = capture->resolved_buffer_classes.back();
                klass.initialization = "external_contract";
                capture->resolved_buffer_values.push_back(
                    {endpoint(dense, false),
                     capture->channels[port],
                     "output_copy",
                     port,
                     external,
                     {{endpoint(dense, false), capture->channels[port], "external_output",
                       "output_copy", port, "write"}},
                     position,
                     position + 1,
                     {}});
            }
        }
        for (std::uint32_t port = 0; port < pnode.output_ports; ++port) {
            const auto slot = slots.output_base + port;
            add_typed_class(slot, "scratch");
            produced[dense].push_back(capture->resolved_buffer_values.size());
            capture->resolved_buffer_values.push_back(
                {endpoint(dense, true),
                 capture->channels[port],
                 "execute",
                 port,
                 slot,
                 {{endpoint(dense, true), capture->channels[port],
                   pnode.kind == graph::GraphRuntimeNodeKind::AudioInput ? "external_input"
                                                                         : "node_output",
                   "execute", port,
                   pnode.kind == graph::GraphRuntimeNodeKind::AudioInput ? "copy" : "write"}},
                 position,
                 position + 1,
                 {}});
        }
    }
    for (const auto& value : capture->resolved_buffer_values) {
        auto found = std::find_if(capture->resolved_buffer_classes.begin(),
                                  capture->resolved_buffer_classes.end(),
                                  [&](const auto& klass) { return klass.id == value.class_id; });
        if (found == capture->resolved_buffer_classes.end())
            return nullptr;
        found->members.insert(found->members.end(), value.accesses.begin(), value.accesses.end());
    }
    capture->typed_resolved_capture = true;

    // Normalize the actual slot classes and per-value processing lifetimes.
    capture->buffer_classes.clear();
    capture->buffer_values.clear();
    std::vector<bool> used_slots(assignment.slot_count, false);
    bool invalid_slot = false;
    const auto add_class = [&](std::uint32_t slot) {
        if (slot < used_slots.size())
            used_slots[slot] = true;
        else
            invalid_slot = true;
    };
    const auto add_value = [&](std::string value) {
        capture->buffer_values.push_back(std::move(value));
    };
    for (std::uint32_t dense = 0; dense < plan.nodes.size(); ++dense) {
        const auto& pnode = plan.nodes[dense];
        const auto& slots = assignment.nodes[dense];
        const auto& key = capture->nodes[authored_positions[dense]].authored_key;
        const auto position = processing_positions[dense];
        const auto sources = serial_snapshot.input_port_sources(dense);
        for (std::uint32_t port = 0; port < pnode.input_ports; ++port) {
            if (port >= sources.size() || port >= capture->channels.size())
                return nullptr;
            const auto mode = sources[port].mode;
            const auto slot = mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace
                                  ? sources[port].slot
                                  : slots.input_base + port;
            add_class(slot);
            add_value(key + ":port=audio:" +
                      (mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace
                           ? "direction=in,role=input_view"
                           : "direction=in,role=input_scratch") +
                      ",channel=" + capture->channels[port] +
                      ",phase=gather,ordinal=" + std::to_string(port) + ",mode=" + mode_name(mode) +
                      ",class=" + std::to_string(slot) + ",lifetime=[" + std::to_string(position) +
                      "," + std::to_string(position + 1) + ")");
        }
        for (std::uint32_t port = 0; port < pnode.output_ports; ++port) {
            if (port >= capture->channels.size())
                return nullptr;
            const auto slot = slots.output_base + port;
            add_class(slot);
            std::size_t last_reader = position;
            for (const auto& connection : plan.connections)
                if (connection.source_index == dense && connection.source_port == port)
                    last_reader =
                        std::max(last_reader, processing_positions[connection.dest_index]);
            if (pnode.kind == graph::GraphRuntimeNodeKind::AudioInput) {
                add_value(key + ":port=audio:direction=out,role=external_input,channel=" +
                          capture->channels[port] +
                          ",phase=execute,ordinal=" + std::to_string(port) +
                          ",mode=copy,class=" + std::to_string(slot) + ",lifetime=[" +
                          std::to_string(position) + "," + std::to_string(last_reader + 1) + ")");
            } else {
                add_value(key + ":port=audio:direction=out,role=node_output,channel=" +
                          capture->channels[port] +
                          ",phase=execute,ordinal=" + std::to_string(port) +
                          ",mode=write,class=" + std::to_string(slot) + ",lifetime=[" +
                          std::to_string(position) + "," + std::to_string(last_reader + 1) + ")");
            }
        }
        if (pnode.kind == graph::GraphRuntimeNodeKind::AudioOutput) {
            for (std::uint32_t port = 0; port < pnode.input_ports; ++port) {
                if (port >= sources.size() || port >= capture->channels.size())
                    return nullptr;
                const auto mode = sources[port].mode;
                const auto slot =
                    mode == format::GraphRuntimeSnapshot::InputPortSource::Mode::InPlace
                        ? sources[port].slot
                        : slots.input_base + port;
                add_class(slot);
                add_value(key + ":port=audio:direction=in,role=external_output,channel=" +
                          capture->channels[port] +
                          ",phase=output_copy,ordinal=" + std::to_string(port) +
                          ",mode=copy,class=" + std::to_string(slot) + ",lifetime=[" +
                          std::to_string(position) + "," + std::to_string(position + 1) + ")");
            }
        }
    }
    for (std::uint32_t slot = 0; slot < used_slots.size(); ++slot)
        if (used_slots[slot])
            capture->buffer_classes.push_back(
                "storage_kind=scratch,capacity_frames=" +
                std::to_string(capture->max_block_frames) +
                ",sample_format=f32,initialization=zero,class=" + std::to_string(slot));
    std::sort(capture->buffer_values.begin(), capture->buffer_values.end());
    if (invalid_slot)
        return nullptr;

    // Slot numbers are executor-local and may change after a harmless
    // allocator renumbering. Assign portable class ordinals in the
    // canonical access order, then rewrite both the class records and
    // every value record to that normalized ordinal.
    std::unordered_map<std::uint32_t, std::uint32_t> normalized_classes;
    const auto normalize_class = [&](std::string& record) {
        const auto marker = record.rfind(",class=");
        if (marker == std::string::npos)
            return false;
        const auto begin = marker + 7;
        const auto end = record.find(',', begin);
        const auto token =
            record.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        std::uint32_t raw = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), raw);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
            return false;
        const auto [it, inserted] =
            normalized_classes.emplace(raw, static_cast<std::uint32_t>(normalized_classes.size()));
        const auto replacement = ",class=" + std::to_string(it->second);
        record.replace(marker, (end == std::string::npos ? record.size() : end) - marker,
                       replacement);
        return true;
    };
    for (auto& value : capture->buffer_values)
        if (!normalize_class(value))
            return nullptr;
    for (auto& value : capture->buffer_classes)
        if (!normalize_class(value))
            return nullptr;
    std::sort(capture->buffer_values.begin(), capture->buffer_values.end());
    const auto class_ordinal = [](const std::string& record) {
        const auto marker = record.rfind(",class=");
        std::uint32_t ordinal = std::numeric_limits<std::uint32_t>::max();
        if (marker != std::string::npos) {
            const auto begin = marker + 7;
            const auto end = record.find(',', begin);
            const auto token =
                record.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            (void)std::from_chars(token.data(), token.data() + token.size(), ordinal);
        }
        return ordinal;
    };
    std::sort(capture->buffer_classes.begin(), capture->buffer_classes.end(),
              [&](const std::string& lhs, const std::string& rhs) {
                  return class_ordinal(lhs) < class_ordinal(rhs);
              });
    return capture;
}

} // namespace pulp::host::detail
