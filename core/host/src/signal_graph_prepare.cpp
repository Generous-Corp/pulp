// SignalGraph private compile/prepare lifecycle seam.
// This translation unit owns candidate construction and publication preparation;
// the public SignalGraph API and the legacy reference walk remain unchanged.
#include "signal_graph_internal.hpp"
#include "signal_graph_port_equaliser.hpp"
#include "spectral_graph_internal.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/host/signal_graph_execution_snapshot.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/runtime/log.hpp>
#include <queue>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulp::host {
namespace {
std::size_t saturating_add(std::size_t a, std::size_t b) {
    const auto max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

std::size_t saturating_mul(std::size_t a, std::size_t b) {
    const auto max = std::numeric_limits<std::size_t>::max();
    if (a == 0 || b == 0)
        return 0;
    return a > max / b ? max : a * b;
}

bool custom_type_matches_node_shape(const CustomNodeType& type, const GraphNode& node) {
    return type.matches_ports(node);
}

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
    // Keep the compiled runtime plan independent of unordered-map iteration
    // and edge discovery order. Authoring position is the semantic tie-break
    // for independent nodes; NodeId is an identity token and may outlive a
    // node's position after removals or future ID allocation changes.
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
            if (c.source_node == current && --in_degree[c.dest_node] == 0)
                queue.push({authoring_index.at(c.dest_node), c.dest_node});
        }
    }
    return order;
}

std::shared_ptr<void> make_custom_instance(const CustomNodeType& type) {
    if (!type.create)
        return nullptr;
    void* raw = type.create();
    if (raw == nullptr)
        return nullptr;
    auto destroy = type.destroy;
    return std::shared_ptr<void>(raw, [destroy](void* p) {
        if (destroy && p)
            destroy(p);
    });
}
} // namespace

void SignalGraph::invalidate_live_locked_() {
    assert_graph_mutation_locked_();
    if (nodes_.empty()) {
        trigger_producers_.clear();
        trigger_envelopes_.clear();
        trigger_audio_.clear();
        trigger_edges_.clear();
        typed_trigger_authoring_.store(false, std::memory_order_release);
    }
    ++authoring_generation_;
    // During a swap-edit the OWNER thread's allow-set mutations must NOT
    // silence the live snapshot — it keeps playing the old compiled graph until
    // prepare_swap() atomically publishes the new one. Any other caller (a second
    // control thread, or the lifecycle / limits / registry mutators, which
    // force-abort the transaction at their top) falls through and invalidates as
    // usual, which safely cancels the no-silence attempt.
    if (in_swap_edit_ && std::this_thread::get_id() == swap_edit_owner_)
        return;
    // Drop the live snapshot; process() will return silence until prepare()
    // is called again. This is the simple, safe semantic: UI-thread edits
    // always require a re-prepare before audio resumes.
    invalidate_trigger_binding_();
    live_slot_.unpublish();
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_locked_();
}

void SignalGraph::clear_prepared_stats_locked_() {
    assert_graph_mutation_locked_();
    prepared_node_count_.store(0, std::memory_order_relaxed);
    prepared_ordered_node_count_.store(0, std::memory_order_relaxed);
    prepared_connection_count_.store(0, std::memory_order_relaxed);
    prepared_total_ports_.store(0, std::memory_order_relaxed);
    prepared_max_block_size_.store(0, std::memory_order_relaxed);
    prepared_node_audio_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_automation_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_delay_buffer_bytes_.store(0, std::memory_order_relaxed);
    prepared_total_buffer_bytes_.store(0, std::memory_order_relaxed);
    transport_suppressed_for_anticipation_.store(0, std::memory_order_relaxed);
}

SignalGraph::PreparedStats SignalGraph::stats_for_snapshot_(const CompiledGraph& cg) {
    std::size_t total_ports = 0;
    for (const auto& [_, shape] : cg.shapes) {
        total_ports += static_cast<std::size_t>(std::max(0, shape.num_input_ports));
        total_ports += static_cast<std::size_t>(std::max(0, shape.num_output_ports));
    }

    std::size_t node_audio_bytes = 0;
    std::size_t automation_bytes = 0;
    for (const auto& [_, rt] : cg.runtime) {
        node_audio_bytes += (rt.output_data.size() + rt.input_data.size()) * sizeof(float);
        automation_bytes += rt.audio_rate_param_data.size() * sizeof(float);
    }

    std::size_t delay_bytes = 0;
    for (const auto& delay : cg.connection_delays) {
        delay_bytes += delay.feedback_prev.size() * sizeof(float);
        if (delay.state)
            delay_bytes += delay.state->ring.size() * sizeof(float);
    }

    const std::size_t total_buffer_bytes = node_audio_bytes + automation_bytes + delay_bytes;

    PreparedStats result;
    result.snapshot_generation = cg.execution_generation;
    result.node_count = cg.runtime.size();
    result.ordered_node_count = cg.ordered_runtime.size();
    result.connection_count = cg.connections.size();
    result.total_ports = total_ports;
    result.max_block_size = cg.max_block_size;
    result.node_audio_buffer_bytes = node_audio_bytes;
    result.automation_buffer_bytes = automation_bytes;
    result.delay_buffer_bytes = delay_bytes;
    result.total_prepared_buffer_bytes = total_buffer_bytes;
    result.spectral_buffer_bytes = cg.spectral ? cg.spectral->charged_bytes : 0;
    return result;
}

void SignalGraph::publish_prepared_stats_locked_(const CompiledGraph& cg) {
    assert_graph_mutation_locked_();
    const auto stats = stats_for_snapshot_(cg);
    prepared_node_count_.store(stats.node_count, std::memory_order_relaxed);
    prepared_ordered_node_count_.store(stats.ordered_node_count, std::memory_order_relaxed);
    prepared_connection_count_.store(stats.connection_count, std::memory_order_relaxed);
    prepared_total_ports_.store(stats.total_ports, std::memory_order_relaxed);
    prepared_max_block_size_.store(stats.max_block_size, std::memory_order_relaxed);
    prepared_node_audio_buffer_bytes_.store(stats.node_audio_buffer_bytes,
                                            std::memory_order_relaxed);
    prepared_automation_buffer_bytes_.store(stats.automation_buffer_bytes,
                                            std::memory_order_relaxed);
    prepared_delay_buffer_bytes_.store(stats.delay_buffer_bytes, std::memory_order_relaxed);
    prepared_total_buffer_bytes_.store(stats.total_prepared_buffer_bytes,
                                       std::memory_order_relaxed);
}

void SignalGraph::compute_latencies_for_(
    CompiledGraph& cg, const std::vector<Connection>& /*conns*/,
    const std::unordered_map<NodeId, PreparedPluginMetadata>& plugin_meta) {
    for (NodeId id : cg.order) {
        auto rt_it = cg.runtime.find(id);
        if (rt_it == cg.runtime.end())
            continue;
        auto& rt = rt_it->second;

        int64_t max_upstream = 0;
        bool has_upstream = false;
        for (const auto& c : cg.connections) {
            if (c.dest_node != id)
                continue;
            // Only latency-aligned audio (plain feedforward or dense audio-rate)
            // contributes to a node's input latency — single-sourced via classify.
            if (!connection_affects_latency(c))
                continue;
            auto src_it = cg.runtime.find(c.source_node);
            if (src_it == cg.runtime.end())
                continue;
            max_upstream = std::max(max_upstream, src_it->second.output_latency);
            has_upstream = true;
        }
        rt.input_latency = has_upstream ? max_upstream : 0;

        int64_t added = 0;
        // 2.2b (H2): read cached latency, never the live slot — latency_samples()
        // reaches into the live plugin (e.g. VST3 getLatencySamples()) and is
        // unsafe concurrent with process() during a swap-time recompile.
        auto mit = plugin_meta.find(id);
        if (mit != plugin_meta.end()) {
            added = std::max<int64_t>(0, mit->second.latency_samples);
        } else if (auto cit = cg.custom_latency_samples.find(id);
                   cit != cg.custom_latency_samples.end()) {
            added = std::max<int64_t>(0, cit->second);
        }
        if (auto cit = cg.custom_latency_samples.find(id); cit != cg.custom_latency_samples.end()) {
            added = cit->second;
        }
        if (cg.spectral) {
            if (const auto* spectral = cg.spectral->find(id);
                spectral && spectral->type == NodeType::SpectralSynthesis)
                added = spectral->identity.fft_size() + spectral->identity.hop();
            if (rt.input_latency + added > std::numeric_limits<int>::max()) {
                cg.total_latency_samples = -1;
                return;
            }
        }
        rt.output_latency = rt.input_latency + added;
    }

    cg.total_latency_samples = 0;
    for (auto& [id, shape] : cg.shapes) {
        if (shape.type != NodeType::AudioOutput)
            continue;
        auto it = cg.runtime.find(id);
        if (it == cg.runtime.end())
            continue;
        cg.total_latency_samples = std::max(cg.total_latency_samples, it->second.input_latency);
    }
    // The host sees one latency for the whole graph. Every audio output is
    // therefore aligned to that total: an output fed only by lower-latency
    // paths receives the difference as delay on its inbound connections, so
    // all host output channels leave the graph together. A graph with one
    // output (or equal-latency outputs) gains no delay from this.
    for (auto& [id, shape] : cg.shapes) {
        if (shape.type != NodeType::AudioOutput)
            continue;
        auto it = cg.runtime.find(id);
        if (it == cg.runtime.end())
            continue;
        it->second.input_latency = cg.total_latency_samples;
        it->second.output_latency = cg.total_latency_samples;
    }

    cg.connection_delays.assign(cg.connections.size(), ConnectionDelay{});
    for (size_t i = 0; i < cg.connections.size(); ++i) {
        const auto& c = cg.connections[i];
        auto src_it = cg.runtime.find(c.source_node);
        auto dst_it = cg.runtime.find(c.dest_node);
        if (src_it == cg.runtime.end() || dst_it == cg.runtime.end())
            continue;

        if (c.feedback) {
            cg.connection_delays[i].feedback_prev.assign(static_cast<size_t>(cg.max_block_size),
                                                         0.0f);
            continue;
        }
        // Non-feedback edges that carry no latency-aligned audio (MIDI, sparse
        // automation) get no delay line — single-sourced via classify().
        if (c.spectral || !connection_affects_latency(c))
            continue;

        int64_t want = dst_it->second.input_latency - src_it->second.output_latency;
        if (want < 0)
            want = 0;
        cg.connection_delays[i].delay_samples = (int)want;
        if (want > 0) {
            auto state = std::make_shared<ConnectionDelay::State>();
            state->ring.assign(static_cast<size_t>((int64_t)cg.max_block_size + want), 0.0f);
            cg.connection_delays[i].state = std::move(state);
        }
    }
}

void SignalGraph::compile_snapshot_for_test(double sample_rate, int max_block_size) {
    // Runs the full compilation path and DROPS the result — no null-first
    // prologue, no publish. See the header: this exercises compile_() concurrently
    // with a live process() for the 2.2a no-silence-swap race contract. Discarding
    // the snapshot frees it here (no reader ever pins it), so this leaks nothing.
    //
    // compile_() writes each authored node's transport_sensitive readback into
    // nodes_ (through node_mut_locked_), so it runs under graph_mutation_mutex_
    // here exactly as it does inside prepare() and prepare_swap(). The audio
    // thread never takes this mutex, so the concurrent process() this hook
    // exists to race against is unaffected; a concurrent control-thread mutator
    // is serialized, which is the same contract every other compile_() caller
    // already gives it.
    GraphMutationLock mutation_lock(*this);
    auto snapshot = compile_(sample_rate, max_block_size);
    (void)snapshot;
}

bool SignalGraph::build_routing_snapshot_locked_(CompiledGraph& cg, bool parallel_safe,
                                                 std::vector<PluginBindingContext>& plugin_ctx,
                                                 std::vector<CustomBindingContext>& custom_ctx,
                                                 format::GraphRuntimeSnapshot& out) {
    const ExecutorSnapshotBinders binders{
        .gain_for = [&cg](NodeId id) -> std::atomic<float>* {
            auto it = cg.runtime.find(id);
            return it == cg.runtime.end() ? nullptr : it->second.gain.get();
        },
        .plugin_for = [&cg](NodeId id) -> PluginSlot* {
            auto it = cg.plugins.find(id);
            return it == cg.plugins.end() ? nullptr : it->second.get();
        },
        // The legacy runtime and routed binding must point at the exact same
        // measurer. Resolve from this candidate snapshot rather than consulting
        // an authoring-side map a second time (important for off-side edits).
        .load_for = [&cg](NodeId id) -> audio::AudioProcessLoadMeasurer* {
            auto it = cg.runtime.find(id);
            return it == cg.runtime.end() ? nullptr : it->second.load;
        },
        .custom_for = [&cg](NodeId id) -> const CustomNodeProcessFn* {
            auto it = cg.custom_processors.find(id);
            return it == cg.custom_processors.end() ? nullptr : &it->second;
        },
        .custom_transport_for = [&cg](NodeId id) -> const CustomNodeTransportProcessFn* {
            auto it = cg.custom_transport_processors.find(id);
            return it == cg.custom_transport_processors.end() ? nullptr : &it->second;
        },
        .custom_event_for = [&cg](NodeId id) -> const CustomNodeEventProcessFn* {
            auto it = cg.custom_event_processors.find(id);
            return it == cg.custom_event_processors.end() ? nullptr : &it->second;
        },
        .custom_latency_for = [&cg](NodeId id) -> int {
            auto it = cg.custom_latency_samples.find(id);
            return it == cg.custom_latency_samples.end() ? 0 : it->second;
        },
        // Cached plugin metadata so the routing build makes no live PluginSlot
        // metadata call (safe for a swap-time recompile).
        .plugin_latency_for = [this](NodeId id) -> int {
            auto it = prepared_plugin_meta_.find(id);
            return it == prepared_plugin_meta_.end() ? 0 : it->second.latency_samples;
        },
        .plugin_params_for = [this](NodeId id) -> const std::vector<HostParamInfo>* {
            auto it = prepared_plugin_meta_.find(id);
            return it == prepared_plugin_meta_.end() ? nullptr : &it->second.parameters;
        },
        .parameter_events_for = [&cg](NodeId id) -> ParameterEventInjectionBinding {
            auto it = cg.runtime.find(id);
            if (it == cg.runtime.end() || !it->second.parameter_input_mailbox) {
                return {};
            }
            return {
                .user_data = &it->second,
                .append = &SignalGraph::append_parameter_mailbox_events_,
                .live_sequence_seen = &it->second.parameter_input_mailbox->sequence_seen,
                .exact_sequence_seen =
                    it->second.exact_parameter_input_mailbox
                        ? &it->second.exact_parameter_input_mailbox->sequence_seen
                        : nullptr,
            };
        },
        .spectral_for = ExecutorSnapshotBinders::SpectralResolver{[&cg](NodeId id) {
            return cg.spectral ? cg.spectral->binding(id) : format::GraphRuntimeNodeBinding{};
        }},
    };
    if (!build_executor_snapshot(cg.executable_nodes, cg.connections, binders, plugin_ctx,
                                 cg.routed.plugin_scratch, out, parallel_safe, &custom_ctx)) {
        return false;
    }
    if (cg.processors.empty())
        return true;

    auto plan = out.plan();
    for (auto& connection : plan.connections) {
        if (connection.dest_index >= plan.nodes.size())
            return false;
        const auto processor = cg.processors.find(plan.nodes[connection.dest_index].id);
        if (processor == cg.processors.end() ||
            connection.kind != graph::GraphRuntimeConnectionKind::Automation) {
            continue;
        }
        const auto* param = processor->second->instance->parameter(connection.automation.param_id);
        if (param != nullptr) {
            connection.automation.bounds_lo = param->range.min;
            connection.automation.bounds_hi = param->range.max;
        }
    }
    std::vector<format::GraphRuntimeNodeBinding> bindings(out.bindings().begin(),
                                                          out.bindings().end());
    for (auto& binding : bindings) {
        const auto processor = cg.processors.find(binding.node_id);
        if (processor == cg.processors.end() || !processor->second)
            continue;
        auto replacement = processor->second->instance->binding(binding.node_id);
        const auto exact_claim = exact_parameter_event_claims_.find(binding.node_id);
        replacement.preserve_parameter_events =
            exact_claim != exact_parameter_event_claims_.end() && !exact_claim->second.expired();
        replacement.load = binding.load;
        binding = replacement;
    }
    return out.reset(std::move(plan), bindings, parallel_safe);
}

std::shared_ptr<SignalGraph::CompiledGraph>
SignalGraph::compile_(double sample_rate, int max_block_size, CompileMode mode) {
    if (connections_.size() != connection_identities_.size()) {
        runtime::log_error("SignalGraph: connection identity metadata is out of sync");
        return nullptr;
    }
    // Trigger generations normally omit the ordinary MIDI egress scratch, but
    // a G-5 host-ingress generation may carry a routed MidiOutput sink.  Keep
    // that fixed scratch available whenever the topology admits one.
    const bool has_midi_output = std::any_of(nodes_.begin(), nodes_.end(), [](const auto& node) {
        return node.type == NodeType::MidiOutput;
    });
    auto cg = std::make_shared<CompiledGraph>(!compiling_trigger_state_ || has_midi_output);
    cg->execution_generation = authoring_generation_;
    if (std::any_of(nodes_.begin(), nodes_.end(),
                    [](const auto& n) { return n.spectral_identity.has_value(); })) {
        cg->spectral = std::make_shared<SpectralRuntimeBank>();
        const auto error = cg->spectral->prepare(nodes_, connections_, sample_rate, max_block_size);
        spectral_error_.store(error, std::memory_order_relaxed);
        if (error != SpectralError::None)
            return nullptr;
    }
    cg->max_block_size = max_block_size;
    if (!cg->spectral)
        cg->legacy_processing_lifetime = processing_handle_lifetime_;
    cg->sample_rate = sample_rate;
    const bool has_control_index = std::any_of(nodes_.begin(), nodes_.end(), [](const auto& n) {
        return n.type == NodeType::Custom && n.custom_type_id == kControlIndexInputTypeId;
    });
    if (graph_event_edge_ || has_control_index) {
        if (graph_event_edge_)
            cg->graph_event_edge = *graph_event_edge_;
        cg->graph_event_lane = std::make_shared<GraphAuthoredEventLane<>>();
        if (cg->graph_event_lane->reset(cg->graph_event_epoch) != GraphEventResult::Accepted)
            return nullptr;
    }
    cg->sample_region_bank = prepared_sample_region_bank_;
    cg->sample_regions = prepared_sample_regions_;
    cg->custom_registry_generation = custom_registry_generation_; // 2.2b predicate (M6)

    std::unordered_map<NodeId, const SampleRegionDefinition*> region_by_member;
    std::unordered_map<NodeId, std::shared_ptr<PreparedSampleRegion>> region_by_anchor;
    for (const auto& definition : sample_region_definitions_) {
        if (definition.input_boundaries.empty())
            return nullptr;
        const NodeId anchor = definition.input_boundaries.front();
        const auto prepared = std::find_if(
            cg->sample_regions.begin(), cg->sample_regions.end(), [&](const auto& region) {
                return region && region->region_id() == definition.region_id;
            });
        if (prepared == cg->sample_regions.end())
            return nullptr;
        region_by_anchor.emplace(anchor, *prepared);
        for (const auto& member : definition.members)
            region_by_member.emplace(member.node, &definition);
    }

    // Prepared-edit identity checks describe the authored graph, including
    // members hidden by the private quotient.  Record those identities before
    // replacing each region with its synthetic executable anchor.
    for (const auto& authored : nodes_) {
        cg->authored_shapes[authored.id] = {authored.type, authored.num_input_ports,
                                            authored.num_output_ports};
        if (authored.type == NodeType::Custom)
            cg->custom_instances[authored.id] = authored.custom_instance.get();
    }

    cg->executable_nodes.reserve(nodes_.size());
    for (const auto& authored : nodes_) {
        const auto member = region_by_member.find(authored.id);
        if (member == region_by_member.end()) {
            cg->executable_nodes.push_back(authored);
            continue;
        }
        const auto& definition = *member->second;
        if (authored.id != definition.input_boundaries.front())
            continue;
        auto anchor = authored;
        anchor.num_input_ports = static_cast<int>(definition.input_boundaries.size());
        anchor.num_output_ports = static_cast<int>(definition.output_boundaries.size());
        anchor.custom_instance.reset();
        anchor.transport_sensitive = true;
        cg->executable_nodes.push_back(std::move(anchor));
    }

    const auto boundary_index = [](const SampleRegionDefinition& definition,
                                   NodeId id) -> std::optional<std::uint32_t> {
        const auto found = std::find_if(definition.members.begin(), definition.members.end(),
                                        [&](const auto& member) { return member.node == id; });
        if (found == definition.members.end() ||
            found->config.kind != SampleKernelConfigKind::BoundaryIndex)
            return std::nullopt;
        return found->config.boundary_index_or_parameter_id;
    };
    cg->connections.reserve(connections_.size());
    cg->connection_identities.reserve(connection_identities_.size());
    for (std::size_t i = 0; i < connections_.size(); ++i) {
        auto connection = connections_[i];
        const auto source_region = region_by_member.find(connection.source_node);
        const auto dest_region = region_by_member.find(connection.dest_node);
        if (source_region != region_by_member.end() && dest_region != region_by_member.end() &&
            source_region->second == dest_region->second) {
            continue;
        }
        if (source_region != region_by_member.end()) {
            const auto index = boundary_index(*source_region->second, connection.source_node);
            if (!index)
                return nullptr;
            connection.source_node = source_region->second->input_boundaries.front();
            connection.source_port = *index;
        }
        if (dest_region != region_by_member.end()) {
            const auto index = boundary_index(*dest_region->second, connection.dest_node);
            if (!index)
                return nullptr;
            connection.dest_node = dest_region->second->input_boundaries.front();
            connection.dest_port = *index;
        }
        cg->connections.push_back(std::move(connection));
        cg->connection_identities.push_back(connection_identities_[i]);
    }

#ifndef NDEBUG
    // 2.2b (H2): every plugin node MUST have captured metadata. A cache miss
    // silently yields 0 latency / empty param-bounds / inert transport (wrong
    // PDC, wrong automation bounds, transport plugin wrongly ahead-rendered) —
    // exactly what this cache prevents. Impossible in prepare()->compile_(), but
    // a future off-thread swap-recompile that added a plugin without re-capturing
    // would fail SILENTLY; assert loudly instead.
    for (const auto& dbg_n : cg->executable_nodes) {
        assert((!dbg_n.plugin || prepared_plugin_meta_.count(dbg_n.id) == 1) &&
               "compile_: plugin node missing from prepared_plugin_meta_ — a swap "
               "recompiled without re-capturing plugin metadata (2.2b H2)");
    }
#endif
    if (!compile_trigger_pair_(*cg))
        return nullptr;
    cg->order = processing_order_for(cg->executable_nodes, cg->connections);
    if (cg->order.size() != cg->executable_nodes.size())
        return nullptr;

    if (!bind_executable_nodes_(*cg, sample_rate, max_block_size, region_by_anchor))
        return nullptr;

    for (size_t ci = 0; ci < cg->connections.size(); ++ci) {
        const auto& c = cg->connections[ci];
        auto rt_it = cg->runtime.find(c.dest_node);
        if (rt_it == cg->runtime.end())
            continue;
        auto src_rt_it = cg->runtime.find(c.source_node);
        NodeRuntime* source_runtime = src_rt_it == cg->runtime.end() ? nullptr : &src_rt_it->second;
        auto& rt = rt_it->second;
        NodeRuntime::EdgeRef edge_ref{ci, source_runtime};
        // Lane bucketing is single-sourced through classify() — the same helper
        // the executor-routing gather uses — so this reference walk and the
        // routed path can never disagree about which lane a Connection carries.
        // sidechain folds into Audio; feedback is orthogonal; the sparse-vs-dense
        // automation split keys off the dense audio_rate flag (the two automation
        // forms are mutually exclusive by construction).
        const ConnectionClass cls = classify(c);
        if (cls.feedback)
            cg->feedback_edges.push_back(edge_ref);
        if (cls.kind == graph::GraphRuntimeConnectionKind::Event && !cls.feedback) {
            rt.inbound_midi_edges.push_back(edge_ref);
        } else if (cls.kind == graph::GraphRuntimeConnectionKind::Audio) {
            rt.inbound_audio_edges.push_back(edge_ref);
        }
        if (cls.kind == graph::GraphRuntimeConnectionKind::Automation && !cls.audio_rate) {
            rt.sparse_automation_edges.push_back(edge_ref);
            auto& ids = rt.sparse_automation_param_ids;
            if (std::find(ids.begin(), ids.end(), c.automation_param_id) == ids.end()) {
                ids.push_back(c.automation_param_id);
                rt.sparse_automation_accum.resize(ids.size());
            }
        }
        if (cls.kind == graph::GraphRuntimeConnectionKind::Automation && cls.audio_rate) {
            rt.audio_rate_modulation_edges.push_back(edge_ref);
            auto& ids = rt.audio_rate_param_ids;
            if (std::find(ids.begin(), ids.end(), c.automation_param_id) == ids.end()) {
                ids.push_back(c.automation_param_id);
                rt.audio_rate_param_data.resize(ids.size() * static_cast<size_t>(max_block_size),
                                                0.0f);
                rt.audio_rate_accum.resize(ids.size());
            }
        }
    }

    // Keep the serial reference walk's ordinary main-bus audio fan-in in the
    // same canonical endpoint order as the routed runtime plan. The reduction
    // is floating-point and therefore order-sensitive; sorting the whole lane
    // would change authored MIDI, automation, feedback, or sidechain order, so
    // only ordinary (feedforward, non-sidechain) audio entries are replaced in
    // their existing positions. The connection index is the deterministic
    // tie-breaker for duplicate endpoint identities, matching the graph plan.
    const auto ordinary_audio = [&](const NodeRuntime::EdgeRef& edge) {
        const auto& connection = cg->connections[edge.connection_index];
        return !connection.feedback && !connection.midi && !connection.automation &&
               !connection.audio_rate_modulation && !connection.sidechain;
    };
    const auto connection_less = [&](const NodeRuntime::EdgeRef& lhs,
                                     const NodeRuntime::EdgeRef& rhs) {
        const auto& left = cg->connections[lhs.connection_index];
        const auto& right = cg->connections[rhs.connection_index];
        const auto left_key = std::tuple{
            left.source_node,
            left.source_port,
            left.dest_node,
            left.dest_port,
        };
        const auto right_key = std::tuple{
            right.source_node,
            right.source_port,
            right.dest_node,
            right.dest_port,
        };
        if (left_key != right_key)
            return left_key < right_key;
        return lhs.connection_index < rhs.connection_index;
    };
    for (auto& [_, rt] : cg->runtime) {
        std::vector<NodeRuntime::EdgeRef> ordinary;
        ordinary.reserve(rt.inbound_audio_edges.size());
        for (const auto& edge : rt.inbound_audio_edges) {
            if (ordinary_audio(edge))
                ordinary.push_back(edge);
        }
        std::sort(ordinary.begin(), ordinary.end(), connection_less);
        std::size_t ordinary_index = 0;
        for (auto& edge : rt.inbound_audio_edges) {
            if (ordinary_audio(edge))
                edge = ordinary[ordinary_index++];
        }
    }

    cg->ordered_runtime.reserve(cg->order.size());
    for (NodeId id : cg->order) {
        auto rt_it = cg->runtime.find(id);
        auto shape_it = cg->shapes.find(id);
        if (rt_it == cg->runtime.end() || shape_it == cg->shapes.end())
            continue;
        cg->ordered_runtime.push_back({
            id,
            shape_it->second,
            &rt_it->second,
        });
    }

    // Prepare this snapshot's per-node live-DSP telemetry store in ordered_runtime
    // order (slot i == ordered_runtime[i]). Enabled state is inherited from the
    // control-thread desired flag so a toggle survives recompiles. Allocation-free
    // once the audio thread is running; all storage is reserved here.
    {
        const auto kind_of = [](NodeType t) noexcept {
            switch (t) {
            case NodeType::AudioInput:
                return audio::LiveDspNodeKind::AudioInput;
            case NodeType::AudioOutput:
                return audio::LiveDspNodeKind::AudioOutput;
            case NodeType::Plugin:
                return audio::LiveDspNodeKind::Plugin;
            case NodeType::Gain:
                return audio::LiveDspNodeKind::Gain;
            case NodeType::MidiInput:
                return audio::LiveDspNodeKind::MidiInput;
            case NodeType::MidiOutput:
                return audio::LiveDspNodeKind::MidiOutput;
            case NodeType::Custom:
                return audio::LiveDspNodeKind::Custom;
            }
            return audio::LiveDspNodeKind::Unknown;
        };
        std::vector<audio::LiveDspNodeInfo> infos;
        infos.reserve(cg->ordered_runtime.size());
        for (const auto& ordered : cg->ordered_runtime) {
            audio::LiveDspNodeInfo info;
            info.node_id = ordered.id;
            info.kind = kind_of(ordered.shape.type);
            info.input_ports =
                static_cast<std::uint32_t>(std::max(0, ordered.shape.num_input_ports));
            info.output_ports =
                static_cast<std::uint32_t>(std::max(0, ordered.shape.num_output_ports));
            info.set_name(to_string(info.kind));
            infos.push_back(info);
        }
        if (!infos.empty()) {
            audio::LiveDspTelemetryConfig tcfg;
            tcfg.spectral_diagnostics = cg->spectral != nullptr;
            cg->live_dsp_telemetry.prepare(tcfg, infos);
            cg->live_dsp_telemetry.set_enabled(
                desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed));
        }
    }

    bind_trigger_pair_(*cg);
    compute_latencies_for_(*cg, cg->connections, prepared_plugin_meta_);
    if (cg->total_latency_samples < 0)
        return nullptr;

    if (!build_routed_for_snapshot_(*cg, sample_rate, max_block_size, mode))
        return nullptr;
    // M5: a SwapNoAnticipation compile must never have built an anticipation lane.
    assert(mode == CompileMode::Normal || !cg->anticipation.valid);
    return cg;
}

int SignalGraph::pump_anticipation(int max_blocks) {
    if (!anticipation_enabled_.load(std::memory_order_relaxed))
        return 0;
    // Single-producer guard: a concurrent or reentrant pump degrades to a no-op
    // rather than corrupting the lane's unsynchronized executor/pool/scratch.
    if (anticipation_pump_busy_.exchange(true, std::memory_order_acquire))
        return 0;
    // Pin the live snapshot like a process() reader (the same RCU handshake
    // Slot::wait_and_clear waits on) so the CompiledGraph object can't be freed
    // while we render its lane. render_ahead is bounded (<= max_blocks), so this
    // never holds the reader count long enough to stall a re-prepare materially.
    //
    // CONTRACT (host-enforced, not provable here): the host MUST stop calling
    // pump_anticipation AND join its producer thread before any prepare()/graph
    // mutation. The reader-pin only keeps the CompiledGraph object alive — it does
    // NOT make the shared PluginSlot instances exclusive, and prepare() reinitializes
    // those same instances (n.plugin->prepare) and builds a new lane over them. A
    // pump concurrent with prepare() would be a data race on the plugin state. This
    // mirrors the existing "no process() concurrent with prepare()" contract.
    int rendered = 0;
    {
        // The pin is released by the guard's destructor before the busy flag
        // clears, so a prepare() waiting on the reader drain cannot observe a
        // stale pin from a pump that has already finished.
        auto read_guard = read_snapshot_();
        if (auto* cg = read_guard.get()) {
            if (cg->anticipation.valid && max_blocks > 0) {
                rendered = cg->anticipation.lane.render_ahead(max_blocks);
            }
        }
    }
    anticipation_pump_busy_.store(false, std::memory_order_release);
    return rendered;
}

// Edit-time plugin parameter list (see header). Cached copy when prepared, else
// the live slot — so a connect during a swap-edit avoids a live parameters() call
// racing process() on that slot.
std::vector<HostParamInfo> SignalGraph::cached_or_live_params_locked_(const GraphNode& n) const {
    assert_graph_mutation_locked_();
    if (const auto processor = processor_nodes_.find(n.id);
        processor != processor_nodes_.end() && processor->second) {
        std::vector<HostParamInfo> params;
        params.reserve(processor->second->instance->parameter_catalog().size());
        for (const auto& source : processor->second->instance->parameter_catalog()) {
            HostParamInfo param;
            param.id = source.id;
            param.name = source.name;
            param.unit = source.unit;
            param.min_value = source.range.min;
            param.max_value = source.range.max;
            param.default_value = source.range.default_value;
            param.rate = source.rate;
            param.flags.stepped = state::is_discrete_param(source);
            param.flags.rampable = !param.flags.stepped;
            param.flags.modulatable = !param.flags.stepped;
            params.push_back(std::move(param));
        }
        return params;
    }
    const auto it = prepared_plugin_meta_.find(n.id);
    if (it != prepared_plugin_meta_.end())
        return it->second.parameters;
    return n.plugin ? n.plugin->parameters() : std::vector<HostParamInfo>{};
}

// Shared preflight for prepare() and (2.2b) prepare_swap(): PURE validation over
// the current nodes_/connections_/limits — the generated-graph limit checks plus
// the audio-rate automation event-capacity gate. Mutates no live state, so
// prepare_swap() can run it before its reinit-free predicate WITHOUT silencing the
// live snapshot (unlike prepare()'s destructive null-first prologue, which stays in
// prepare()). Caller holds graph_mutation_mutex_. Returns false (with a logged
// reason) on any rejection; true if the graph passes every gate.
bool SignalGraph::preflight_locked_(int max_block_size) {
    assert_graph_mutation_locked_();
    // Typed factories can only enter through their explicit prepared pair.
    // Generic topology-edit clones do not carry the controller or event ledger.
    if (!compiling_trigger_state_ &&
        std::any_of(nodes_.begin(), nodes_.end(), [](const auto& node) {
            return node.custom_type_id.starts_with("pulp.trigger.");
        }))
        return false;
    // Scalar-only kernels have no block execution fallback.
    if (sample_region_definitions_.empty() && has_sample_kernel_nodes_locked_())
        return false;
    const auto generated_validation = validate_generated_graph(max_block_size);
    switch (generated_validation.reason) {
    case GeneratedGraphValidationRejectReason::None:
        break;
    case GeneratedGraphValidationRejectReason::InvalidBlockSize:
        return false;
    case GeneratedGraphValidationRejectReason::MaxBlockSizeExceeded:
        runtime::log_error("SignalGraph: max block size {} exceeds configured limit {}",
                           generated_validation.actual, generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::NodeLimitExceeded:
        runtime::log_error("SignalGraph: node count {} exceeds configured limit {}",
                           generated_validation.actual, generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::ConnectionLimitExceeded:
        runtime::log_error("SignalGraph: connection count {} exceeds configured limit {}",
                           generated_validation.actual, generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::PortLimitExceeded:
        runtime::log_error("SignalGraph: port count {} exceeds configured limit {}",
                           generated_validation.actual, generated_validation.limit);
        return false;
    case GeneratedGraphValidationRejectReason::EstimatedWorkExceeded:
        runtime::log_error("SignalGraph: estimated work units {} exceed configured limit {}",
                           generated_validation.actual, generated_validation.limit);
        return false;
    }

    std::unordered_map<NodeId, std::vector<uint32_t>> sparse_params_by_node;
    std::unordered_map<NodeId, std::vector<uint32_t>> audio_rate_params_by_node;
    auto add_unique_param = [](std::vector<uint32_t>& params, uint32_t param_id) {
        if (std::find(params.begin(), params.end(), param_id) == params.end()) {
            params.push_back(param_id);
        }
    };
    for (const auto& c : connections_) {
        if (c.automation) {
            add_unique_param(sparse_params_by_node[c.dest_node], c.automation_param_id);
        }
        if (c.audio_rate_modulation) {
            add_unique_param(audio_rate_params_by_node[c.dest_node], c.automation_param_id);
        }
    }
    std::size_t dense_lane_count = 0;
    for (const auto& [node_id, audio_rate_params] : audio_rate_params_by_node) {
        const auto sparse_it = sparse_params_by_node.find(node_id);
        const size_t sparse_count =
            sparse_it == sparse_params_by_node.end() ? 0 : sparse_it->second.size();
        const auto processor = processor_nodes_.find(node_id);
        const bool consumes_dense = processor != processor_nodes_.end() && processor->second &&
                                    processor->second->instance->descriptor()
                                        .effective_capabilities()
                                        .consumes_audio_rate_modulations;
        if (consumes_dense) {
            if (max_block_size >
                    static_cast<int>(format::GraphRuntimeAutomationScratch::kMaxDenseFrames) ||
                audio_rate_params.size() >
                    format::GraphRuntimeAutomationScratch::kMaxDenseLanesPerNode) {
                return false;
            }
            dense_lane_count = saturating_add(dense_lane_count, audio_rate_params.size());
            if (dense_lane_count > format::GraphRuntimeAutomationScratch::kMaxDenseLanesPerGraph) {
                return false;
            }
            continue;
        }
        const size_t required_events =
            audio_rate_params.size() * static_cast<size_t>(max_block_size) + sparse_count * 2;
        if (required_events > ParameterEventQueue::kCapacity) {
            runtime::log_error("SignalGraph: audio-rate modulation for node {} requires {} "
                               "parameter events (capacity {})",
                               node_id, required_events, ParameterEventQueue::kCapacity);
            return false;
        }
    }
    return true;
}

bool SignalGraph::prepare(double sample_rate, int max_block_size) {
    return prepare_impl_(sample_rate, max_block_size, nullptr);
}

bool SignalGraph::plugin_restart_pending() const {
    GraphMutationLock mutation_lock(*this);
    for (const auto& n : nodes_) {
        if (n.type != NodeType::Plugin || !n.plugin)
            continue;
        const auto meta = prepared_plugin_meta_.find(n.id);
        if (meta == prepared_plugin_meta_.end())
            continue;
        if (n.plugin->restart_request_count() != meta->second.restart_requests)
            return true;
    }
    return false;
}

bool SignalGraph::apply_plugin_restarts() {
    double rate = 0.0;
    int block = 0;
    {
        GraphMutationLock mutation_lock(*this);
        rate = last_plugin_prepare_rate_;
        block = last_plugin_prepare_block_;
    }
    if (!plugin_restart_pending())
        return true;
    if (!(rate > 0.0) || block <= 0)
        return false;
    return prepare(rate, block);
}

void SignalGraph::set_hosted_plugin_process_mode(format::ProcessMode mode) {
    GraphMutationLock mutation_lock(*this);
    hosted_plugin_process_mode_ = mode;
}

format::ProcessMode SignalGraph::hosted_plugin_process_mode() const {
    GraphMutationLock mutation_lock(*this);
    return hosted_plugin_process_mode_;
}

NodeId SignalGraph::last_prepare_custom_failure_node() const {
    GraphMutationLock mutation_lock(*this);
    return last_prepare_custom_failure_node_;
}

bool SignalGraph::prepare_impl_(double sample_rate, int max_block_size,
                                const PrepareLifecycleObserver* lifecycle_observer) {
    // Serialize the ENTIRE prepare against concurrent control-thread mutators
    // (set_node_gain / add_*/remove_node, which all run on the UI thread). The
    // lock covers two distinct shared surfaces:
    //   1. The source topology — nodes_ iteration + GraphNode plain-field reads,
    //      including GraphNode::gain in compile_() — vs the mutators' writes.
    //   2. The snapshot-publication state (live_slot_)
    //      mutated by the prologue's retire_snapshot_ + the epilogue's
    //      publish/prune, which a concurrent mutator's invalidate_live_locked_() also
    //      touches. Those were previously single-control-thread-owned; with a
    //      second control thread editing the graph they must be serialized too.
    //
    // Deadlock-free: prepare() only ever drives the NON-blocking
    // Slot::reclaim_if_quiescent() (never the blocking Slot::wait_and_clear),
    // and the one place a thread holds a Slot reader pin AND wants this
    // mutex — set_node_gain() — releases the mutex before pinning, so this lock can
    // never invert order with the reader-drain handshake.
    GraphMutationLock mutation_lock(*this);
    for (const auto& node : nodes_) {
        if (!node.plugin || !is_isolated_plugin_slot(*node.plugin))
            continue;
        if (node.num_input_ports != 2 || node.num_output_ports != 2) {
            runtime::log_error("SignalGraph: isolated CLAP requires stereo node ports");
            return false;
        }
        for (const auto& connection : connections_) {
            if ((connection.source_node == node.id || connection.dest_node == node.id) &&
                (connection.midi || connection.automation || connection.audio_rate_modulation ||
                 connection.sidechain)) {
                runtime::log_error("SignalGraph: isolated CLAP refuses event and sidechain edges");
                return false;
            }
        }
    }
    // Refuse before unpublishing or invoking any slot lifecycle method.
    if (has_isolated_plugins_locked_() &&
        (sample_rate != 48000.0 || max_block_size != 64 || !canonical_executor_routing_enabled() ||
         parallel_routing_enabled() || anticipation_enabled() ||
         hosted_plugin_process_mode_ != format::ProcessMode::Realtime || live_slot_.has_value() ||
         processing_handle_lifetime_.use_count() != 1)) {
        runtime::log_error(
            "SignalGraph: isolated CLAP requires a fresh serial canonical 48k/64 graph");
        return false;
    }

    if ((!trigger_producers_.empty() || !trigger_envelopes_.empty() || !trigger_audio_.empty() ||
         !trigger_edges_.empty()) &&
        !compiling_trigger_state_)
        return false;
    if (spectral_mode_.load(std::memory_order_acquire)) {
        spectral_error_.store(SpectralError::UnsupportedOperation, std::memory_order_relaxed);
        return false;
    }
    const bool wants_spectral = std::any_of(nodes_.begin(), nodes_.end(), [](const auto& node) {
        return node.spectral_identity.has_value();
    });
    if (wants_spectral && (processing_handle_lifetime_.use_count() != 1 || live_slot_.has_value() ||
                           spectral_terminal_)) {
        spectral_error_.store(SpectralError::RequiresStoppedController, std::memory_order_relaxed);
        return false;
    }
    ++authoring_generation_;
    last_prepare_custom_failure_node_ = 0;

    cancel_swap_edit_locked_();

    live_slot_.unpublish();
    total_latency_samples_.store(0, std::memory_order_relaxed);
    clear_prepared_stats_locked_();

    // Generated-graph limits + audio-rate automation event-capacity gate (H3 —
    // shared with prepare_swap()). prepare() has already nulled the live snapshot
    // above, so a preflight failure here leaves the graph silent (existing
    // behavior); prepare_swap() runs the same preflight BEFORE any mutation.
    if (!preflight_locked_(max_block_size))
        return false;

    const bool has_prebuilt_edit_regions =
        prepared_edit_origin_ != nullptr && prepared_sample_region_bank_ != nullptr &&
        prepared_sample_regions_.size() == sample_region_definitions_.size();
    if (!has_prebuilt_edit_regions) {
        prepared_sample_region_bank_.reset();
        prepared_sample_regions_.clear();
    }
    if (sample_region_definitions_.empty()) {
        if (has_sample_kernel_nodes_locked_())
            return false;
    } else if (!has_prebuilt_edit_regions) {
        sample_region_proof_block_size_ =
            max_block_size > 0 ? static_cast<std::uint32_t>(max_block_size) : 0;
        if (!std::isfinite(sample_rate) || sample_rate <= 0.0)
            return false;
        try {
            std::vector<SampleRegionCandidate> candidates;
            std::vector<PreparedSampleRegionPlan> plans;
            candidates.reserve(sample_region_definitions_.size());
            plans.reserve(sample_region_definitions_.size());
            for (const auto& definition : sample_region_definitions_) {
                if (!sample_region_metadata_proof_locked_(definition).accepted)
                    return false;
                candidates.push_back(sample_region_candidate_locked_(definition));
            }
            if (!pulp::host::prove_sample_regions(candidates).accepted ||
                !sample_region_exterior_proof_locked_().accepted)
                return false;
            for (const auto& node : nodes_) {
                if (node.type == NodeType::Custom &&
                    sample_kernel_type(node.custom_type_id, node.custom_type_version) != nullptr &&
                    sample_region_for_node_locked_(node.id) == 0)
                    return false;
            }
            for (const auto& candidate : candidates) {
                auto result = build_sample_region_plan(candidate);
                if (!result.proof.accepted || !result.plan)
                    return false;
                plans.push_back(std::move(*result.plan));
            }
            const auto contract =
                SampleRegionParameterContract::from_regions(sample_region_definitions_);
            if (!contract.valid() || sample_region_parameter_binding_ == nullptr ||
                !contract.matches_promoted(sample_region_parameter_binding_->contract()))
                return false;
            const auto generation = std::max<std::uint64_t>(1, authoring_generation_);
            auto bank = SampleRegionStateBank::create_fresh(
                plans, sample_rate, static_cast<std::uint32_t>(max_block_size), generation);
            if (!bank)
                return false;
            std::vector<std::shared_ptr<PreparedSampleRegion>> prepared;
            prepared.reserve(plans.size());
            for (auto& plan : plans) {
                auto region = PreparedSampleRegion::create(std::move(plan), bank,
                                                           sample_region_parameter_binding_);
                if (!region)
                    return false;
                prepared.push_back(std::move(region));
            }
            prepared_sample_region_bank_ = std::move(bank);
            prepared_sample_regions_ = std::move(prepared);
        } catch (...) {
            prepared_sample_region_bank_.reset();
            prepared_sample_regions_.clear();
            return false;
        }
    }

    // Prepare each plugin slot first (pre-compile step). Immediately capture
    // each slot's metadata (params/latency/transport) into prepared_plugin_meta_
    // so compile_() and the executor-routing build read the cache instead of
    // calling live PluginSlot metadata methods — the contract the no-silence
    // swap (2.2b) relies on (H2). Safe to read here: null-first prepare, mutation
    // lock held, no concurrent process() on these instances.
    prepared_plugin_meta_.clear();
    for (auto& n : nodes_) {
        if (n.plugin) {
            if (lifecycle_observer != nullptr &&
                lifecycle_observer->plugin_will_prepare != nullptr) {
                lifecycle_observer->plugin_will_prepare(lifecycle_observer->context,
                                                        n.plugin.get());
            }
            {
                auto settings = n.plugin->host_settings();
                if (settings.process_mode != hosted_plugin_process_mode_) {
                    settings.process_mode = hosted_plugin_process_mode_;
                    n.plugin->set_host_settings(settings);
                }
            }
            if (!n.plugin->prepare(sample_rate, max_block_size)) {
                runtime::log_error("SignalGraph: failed to prepare plugin '{}'", n.name);
                return false;
            }
            // Read the restart count AFTER prepare: a latency change reported
            // during activation is already reflected in latency_samples().
            prepared_plugin_meta_[n.id] = PreparedPluginMetadata{
                n.plugin->parameters(), std::max(0, n.plugin->latency_samples()),
                n.plugin->wants_transport(), n.plugin->restart_request_count()};
        }
    }
    last_plugin_prepare_rate_ = sample_rate;
    last_plugin_prepare_block_ = max_block_size;

    for (const auto& n : nodes_) {
        const auto processor = processor_nodes_.find(n.id);
        if (processor == processor_nodes_.end() || !processor->second)
            continue;
        format::PrepareContext context;
        context.sample_rate = sample_rate;
        context.max_buffer_size = max_block_size;
        context.input_channels = n.num_input_ports;
        context.output_channels = n.num_output_ports;
        if (!processor->second->instance->prepare(context)) {
            runtime::log_error("SignalGraph: failed to prepare Processor node '{}'", n.name);
            return false;
        }
        // ProcessorNode deliberately reuses the Plugin topology kind. Cache
        // its prepare-stable latency in the same metadata snapshot consumed by
        // both the legacy walk and executor PDC pass; reading the live
        // processor during compile would reintroduce the swap-time race this
        // cache avoids.
        prepared_plugin_meta_[n.id] = PreparedPluginMetadata{
            {}, std::max(0, processor->second->instance->processor().latency_samples()), false};
    }

    // Create/prepare stateful custom-node instances on this UI thread before
    // the snapshot is published, mirroring the plugin step above. A
    // freshly-loaded state blob is applied exactly once via load_state.
    for (auto& n : nodes_) {
        if (n.type != NodeType::Custom)
            continue;
        const CustomNodeType* type = custom_node_type(n.custom_type_id, n.custom_type_version);
        if (type == nullptr || !type->create || !custom_type_matches_node_shape(*type, n)) {
            continue; // stateless / unresolved / shape-mismatch: no instance
        }
        if (!n.custom_instance) {
            n.custom_instance = make_custom_instance(*type);
            if (!n.custom_instance) {
                last_prepare_custom_failure_node_ = n.id;
                runtime::log_error("SignalGraph: failed to create instance for custom node '{}'",
                                   n.name);
                return false;
            }
        }
        if (n.custom_instance) {
            if (n.custom_state_pending && type->load_state) {
                if (!type->load_state(n.custom_instance.get(), n.custom_state_blob)) {
                    last_prepare_custom_failure_node_ = n.id;
                    runtime::log_error("SignalGraph: failed to restore state for custom node '{}'",
                                       n.name);
                    return false;
                }
                n.custom_state_pending = false;
            }
            if (type->prepare) {
                if (lifecycle_observer != nullptr &&
                    lifecycle_observer->custom_will_prepare != nullptr) {
                    lifecycle_observer->custom_will_prepare(lifecycle_observer->context,
                                                            n.custom_instance.get());
                }
                type->prepare(n.custom_instance.get(), sample_rate, max_block_size);
            }
        }
    }

    auto cg = compile_(sample_rate, max_block_size);
    if (!cg)
        return false;
    total_latency_samples_.store(cg->total_latency_samples, std::memory_order_relaxed);
    publish_prepared_stats_locked_(*cg);
    if (cg->spectral && prepared_edit_origin_ == nullptr) {
        if (diagnostic_graph_identity_ > std::numeric_limits<std::uint32_t>::max()) {
            spectral_error_.store(SpectralError::EpochExhausted, std::memory_order_relaxed);
            return false;
        }
        spectral_slots_[0] = std::move(cg);
        spectral_active_.store(0, std::memory_order_seq_cst);
        spectral_render_generation_ = authoring_generation_;
        spectral_mode_.store(true, std::memory_order_release);
    } else {
        live_slot_.publish(std::move(cg));
    }
    return true;
}

void SignalGraph::set_limits(GraphLimits limits) {
    // Mutates limits_ (read by validate_generated_graph under the lock) and drives
    // invalidate_live_locked_(); serialize against a concurrent prepare()/mutator.
    GraphMutationLock mutation_lock(*this);
    if (refuse_spectral_mutation_())
        return;
    cancel_swap_edit_locked_();
    limits_ = limits;
    invalidate_live_locked_();
}

std::size_t SignalGraph::total_declared_ports_locked_() const {
    std::size_t port_count = 0;
    for (const auto& n : nodes_) {
        port_count += static_cast<std::size_t>(std::max(0, n.num_input_ports));
        port_count += static_cast<std::size_t>(std::max(0, n.num_output_ports));
    }
    return port_count;
}

std::size_t SignalGraph::estimate_generated_graph_work_units(int max_block_size) const {
    if (max_block_size <= 0)
        return 0;

    const std::size_t block = static_cast<std::size_t>(max_block_size);
    const std::size_t port_count = total_declared_ports_locked_();
    std::size_t dense_edges = 0;
    std::size_t sparse_edges = 0;
    for (const auto& c : connections_) {
        if (c.audio_rate_modulation)
            ++dense_edges;
        if (c.automation && !c.audio_rate_modulation)
            ++sparse_edges;
    }

    std::size_t work = 0;
    work = saturating_add(work, saturating_mul(nodes_.size(), 16));
    work = saturating_add(work, saturating_mul(connections_.size(), 8));
    work = saturating_add(work, saturating_mul(port_count, block));
    work = saturating_add(work, saturating_mul(dense_edges, block));
    work = saturating_add(work, saturating_mul(sparse_edges, 2));
    return work;
}

SignalGraph::GeneratedGraphValidation
SignalGraph::validate_generated_graph(int max_block_size) const {
    if (max_block_size <= 0) {
        return {
            false,
            GeneratedGraphValidationRejectReason::InvalidBlockSize,
            max_block_size < 0 ? 0 : static_cast<std::size_t>(max_block_size),
            1,
        };
    }
    if (limits_.max_block_size > 0 && max_block_size > limits_.max_block_size) {
        return {
            false,
            GeneratedGraphValidationRejectReason::MaxBlockSizeExceeded,
            static_cast<std::size_t>(max_block_size),
            static_cast<std::size_t>(limits_.max_block_size),
        };
    }
    if (nodes_.size() > limits_.max_nodes) {
        return {
            false,
            GeneratedGraphValidationRejectReason::NodeLimitExceeded,
            nodes_.size(),
            limits_.max_nodes,
        };
    }
    if (connections_.size() > limits_.max_connections) {
        return {
            false,
            GeneratedGraphValidationRejectReason::ConnectionLimitExceeded,
            connections_.size(),
            limits_.max_connections,
        };
    }
    const std::size_t port_count = total_declared_ports_locked_();
    if (port_count > limits_.max_ports) {
        return {
            false,
            GeneratedGraphValidationRejectReason::PortLimitExceeded,
            port_count,
            limits_.max_ports,
        };
    }
    const std::size_t estimated_work = estimate_generated_graph_work_units(max_block_size);
    if (limits_.max_estimated_work_units > 0 && estimated_work > limits_.max_estimated_work_units) {
        return {
            false,
            GeneratedGraphValidationRejectReason::EstimatedWorkExceeded,
            estimated_work,
            limits_.max_estimated_work_units,
        };
    }
    return {};
}

} // namespace pulp::host
