// SignalGraph private compile/prepare lifecycle seam.
// This translation unit owns candidate construction and publication preparation;
// the public SignalGraph API and the legacy reference walk remain unchanged.
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
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph.hpp>
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
    return type.num_input_ports == node.num_input_ports &&
           type.num_output_ports == node.num_output_ports;
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

void SignalGraph::publish_prepared_stats_locked_(const CompiledGraph& cg) {
    assert_graph_mutation_locked_();
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

    prepared_node_count_.store(cg.runtime.size(), std::memory_order_relaxed);
    prepared_ordered_node_count_.store(cg.ordered_runtime.size(), std::memory_order_relaxed);
    prepared_connection_count_.store(cg.connections.size(), std::memory_order_relaxed);
    prepared_total_ports_.store(total_ports, std::memory_order_relaxed);
    prepared_max_block_size_.store(cg.max_block_size, std::memory_order_relaxed);
    prepared_node_audio_buffer_bytes_.store(node_audio_bytes, std::memory_order_relaxed);
    prepared_automation_buffer_bytes_.store(automation_bytes, std::memory_order_relaxed);
    prepared_delay_buffer_bytes_.store(delay_bytes, std::memory_order_relaxed);
    prepared_total_buffer_bytes_.store(total_buffer_bytes, std::memory_order_relaxed);
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
        if (!connection_affects_latency(c))
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
    auto cg = std::make_shared<CompiledGraph>();
    cg->max_block_size = max_block_size;
    cg->sample_rate = sample_rate;
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
    cg->order = processing_order_for(cg->executable_nodes, cg->connections);
    if (cg->order.size() != cg->executable_nodes.size())
        return nullptr;

    for (auto& n : cg->executable_nodes) {
        NodeRuntime rt;
        // Resolve (or lazily create) this node's persistent load measurer.
        // node_load_ only grows here, so the raw measurer pointer handed to the
        // audio thread via NodeRuntime::load stays valid across snapshot swaps.
        // Locked against a concurrent node_loads() poll on the UI thread.
        if (prepared_edit_origin_ != nullptr) {
            std::lock_guard<std::mutex> origin_load_lock(prepared_edit_origin_->node_load_mu_);
            const auto existing = prepared_edit_origin_->node_load_.find(n.id);
            if (existing != prepared_edit_origin_->node_load_.end()) {
                rt.load = existing->second.get();
            }
        }
        if (rt.load == nullptr) {
            std::lock_guard<std::mutex> node_load_lock(node_load_mu_);
            auto& load_slot = node_load_[n.id];
            if (!load_slot) {
                load_slot = std::make_unique<audio::AudioProcessLoadMeasurer>();
            }
            rt.load = load_slot.get();
        }
        const int out_ch = std::max(0, n.num_output_ports);
        const int in_ch = std::max(0, n.num_input_ports);
        rt.output_data.assign(static_cast<size_t>(out_ch) * max_block_size, 0.f);
        rt.input_data.assign(static_cast<size_t>(in_ch) * max_block_size, 0.f);
        rt.output_ptrs.resize(out_ch);
        rt.input_ptrs.resize(in_ch);
        rt.input_const_ptrs.resize(in_ch);
        for (int c = 0; c < out_ch; ++c)
            rt.output_ptrs[c] = rt.output_data.data() + static_cast<size_t>(c) * max_block_size;
        for (int c = 0; c < in_ch; ++c) {
            rt.input_ptrs[c] = rt.input_data.data() + static_cast<size_t>(c) * max_block_size;
            rt.input_const_ptrs[c] = rt.input_ptrs[c];
        }
        rt.gain = std::make_unique<std::atomic<float>>(n.gain);
        if (n.plugin) {
            // 2.2b (H2): read cached parameter bounds, not the live slot.
            auto mit = prepared_plugin_meta_.find(n.id);
            if (mit != prepared_plugin_meta_.end()) {
                for (const auto& p : mit->second.parameters) {
                    rt.param_bounds.push_back({
                        p.id,
                        p.min_value,
                        p.max_value,
                    });
                }
            }
        }
        auto [runtime_it, inserted] = cg->runtime.emplace(n.id, std::move(rt));
        (void)inserted;
        prepare_midi_block_storage(runtime_it->second.midi_in, runtime_it->second.midi_in_ump);
        prepare_midi_block_storage(runtime_it->second.midi_out, runtime_it->second.midi_out_ump);
        if (n.type == NodeType::MidiInput) {
            // Keep mailbox identity across a live snapshot recompile so an
            // injection published between its build and publication cannot fall
            // between the old and new snapshots. Normal eager prepare has no live
            // snapshot here and allocates a fresh mailbox.
            if (const auto live = live_slot_.live()) {
                auto old_it = live->runtime.find(n.id);
                if (old_it != live->runtime.end()) {
                    runtime_it->second.midi_input_mailbox = old_it->second.midi_input_mailbox;
                }
            }
            if (!runtime_it->second.midi_input_mailbox) {
                runtime_it->second.midi_input_mailbox = std::make_shared<MidiInputMailbox>();
            }
        }
        if (n.type == NodeType::MidiOutput) {
            runtime_it->second.midi_output_mailbox = std::make_unique<MidiOutputMailbox>();
        }
        if (n.type == NodeType::Plugin &&
            (n.plugin != nullptr || processor_nodes_.contains(n.id))) {
            const auto exact_claim = exact_parameter_event_claims_.find(n.id);
            const bool exact_claimed = exact_claim != exact_parameter_event_claims_.end() &&
                                       !exact_claim->second.expired();
            const auto exact_owner = exact_claimed
                                         ? exact_claim->second.lock()
                                         : std::shared_ptr<detail::ExactParameterIngressOwner>{};
            // Keep mailbox identity across a live snapshot recompile so a
            // batch published while prepare_swap() is building cannot fall
            // between the old and new snapshots. Normal eager prepare has no
            // live snapshot here and allocates a fresh mailbox.
            if (const auto live = live_slot_.live()) {
                auto old_it = live->runtime.find(n.id);
                const auto old_exact_owner =
                    old_it != live->runtime.end()
                        ? old_it->second.exact_parameter_event_owner.lock()
                        : std::shared_ptr<detail::ExactParameterIngressOwner>{};
                if (old_it != live->runtime.end() && old_exact_owner == exact_owner) {
                    runtime_it->second.parameter_input_mailbox =
                        old_it->second.parameter_input_mailbox;
                }
            }
            if (!runtime_it->second.parameter_input_mailbox) {
                runtime_it->second.parameter_input_mailbox =
                    std::make_shared<ParameterInputMailbox>();
            }
            if (exact_claimed) {
                runtime_it->second.exact_parameter_input_mailbox =
                    std::make_unique<ParameterInputMailbox>();
                runtime_it->second.exact_parameter_event_owner = exact_claim->second;
            }
        }

        CompiledGraph::NodeShape shape{n.type, n.num_input_ports, n.num_output_ports};
        cg->shapes[n.id] = shape;

        if (n.plugin)
            cg->plugins[n.id] = n.plugin;
        if (const auto processor = processor_nodes_.find(n.id);
            processor != processor_nodes_.end() && processor->second) {
            cg->processors[n.id] = processor->second;
        }
        // Resolve the node's transport-sensitivity ONCE here (before the
        // anticipation eligibility analysis and the routed-snapshot build, both
        // later in compile_). The SAME GraphNode::transport_sensitive value feeds
        // the anticipation partition (seeds AnticipationExclusion::TransportSensitive)
        // and the routed binding (PluginBindingContext::wants_transport /
        // CustomBindingContext::process_transport), so the two can never disagree.
        // Prepare-stable: a slot/type whose capability changes later needs a
        // re-prepare to be observed.
        n.transport_sensitive = region_by_anchor.contains(n.id);
        if (n.type == NodeType::Plugin) {
            // 2.2b (H2): read cached transport-sensitivity, not the live slot.
            auto mit = prepared_plugin_meta_.find(n.id);
            n.transport_sensitive =
                (mit != prepared_plugin_meta_.end()) && mit->second.wants_transport;
        }
        if (n.type == NodeType::Custom) {
            if (const auto* type = custom_node_type(n.custom_type_id, n.custom_type_version);
                type && custom_type_matches_node_shape(*type, n)) {
                if (n.custom_instance && type->process_instance) {
                    // Bind the stateful processor. The lambda captures the
                    // instance shared_ptr BY VALUE, so this snapshot keeps the
                    // instance alive for its whole audio-thread lifetime (same
                    // guarantee as cg->plugins[...] for plugin nodes). No raw
                    // pointer into GraphNode is stored.
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance;
                    cg->custom_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in,
                                   int num_samples) { fn(inst.get(), out, in, num_samples); };
                } else if (type->process) {
                    cg->custom_processors[n.id] = type->process;
                }
                // Event-aware resolution, with the same instance-lifetime
                // guarantee: a stateful callback is wrapped in a lambda holding
                // the instance shared_ptr by value, so the map always stores the
                // stateless shape and both execution paths read one map.
                if (n.custom_instance && type->process_instance_events) {
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance_events;
                    cg->custom_event_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in, int num_samples,
                                   const CustomNodeEventBlock& events) {
                            fn(inst.get(), out, in, num_samples, events);
                        };
                } else if (type->process_events) {
                    cg->custom_event_processors[n.id] = type->process_events;
                }
                // A registered type with no live callback is transparent on the
                // live graph, so it must not add latency there. Baked-only
                // callbacks capture their latency separately during lowering.
                // Evaluated once here, off the audio thread, at the graph's own
                // rate, and clamped into the declared range — the value is not
                // knowable at registration, so this is where it gets checked.
                if ((cg->custom_processors.contains(n.id) ||
                     cg->custom_event_processors.contains(n.id)) &&
                    (type->latency_samples_for_block || type->latency_samples)) {
                    const int latency =
                        type->latency_samples_for_block
                            ? type->latency_samples_for_block(sample_rate, max_block_size)
                            : type->latency_samples(sample_rate);
                    cg->custom_latency_samples[n.id] =
                        std::clamp(latency, 0, CustomNodeType::kMaxLatencySamples);
                }
                // Bake-layer param injection: if the type declared baked_params
                // and a param-aware process, bind a closure that captures the
                // instance shared_ptr by value (keepalive, same guarantee as the
                // plain closure above). bake() copies this so a baked graph can
                // deliver injected ParameterEvents to the node. The live routed
                // path never calls it — it runs process_instance as usual.
                if (n.custom_instance && type->process_instance_baked_param &&
                    !type->baked_params.empty()) {
                    auto inst = n.custom_instance;
                    auto pfn = type->process_instance_baked_param;
                    cg->custom_param_processors[n.id] =
                        [inst, pfn](audio::BufferView<float>& out,
                                    const audio::BufferView<const float>& in, int num_samples,
                                    const BakedParamView& params) {
                            pfn(inst.get(), out, in, num_samples, params);
                        };
                }
                // Transport-aware resolution mirrors the plain one: prefer the
                // stateful variant when an instance + stateful callback exist,
                // else the stateless variant. Any non-empty result marks the node
                // transport-sensitive (the presence of the map entry mirrors the
                // flag, both resolved from this one condition).
                if (n.custom_instance && type->process_instance_transport) {
                    auto inst = n.custom_instance;
                    auto fn = type->process_instance_transport;
                    cg->custom_transport_processors[n.id] =
                        [inst, fn](audio::BufferView<float>& out,
                                   const audio::BufferView<const float>& in, int num_samples,
                                   const format::ProcessContext& transport) {
                            fn(inst.get(), out, in, num_samples, transport);
                        };
                    n.transport_sensitive = true;
                } else if (type->process_transport) {
                    cg->custom_transport_processors[n.id] = type->process_transport;
                    n.transport_sensitive = true;
                }
            }
        }
        if (const auto region = region_by_anchor.find(n.id); region != region_by_anchor.end()) {
            auto prepared = region->second;
            cg->custom_processors[n.id] =
                [prepared](audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                           int frames) noexcept { prepared->process(out, in, frames); };
            cg->custom_latency_samples[n.id] = 0;
            // A region anchor's processor IS the prepared region, which has no
            // event plane and no transport plane. Drop both bindings its
            // registered type may have resolved above, so the region cannot be
            // entered through either lane. The transport entry matters because
            // the routed binding tries transport BEFORE the plain callback while
            // the reference walk has no transport branch at all, so leaving one
            // here would run the transport callback on one path and the region on
            // the other.
            cg->custom_event_processors.erase(n.id);
            cg->custom_transport_processors.erase(n.id);
            n.transport_sensitive = true;
        }
        // Before sample-region quotienting, compile_ resolved this flag directly
        // on nodes_. Keep that public authoring readback stable now that the
        // executable topology is a private copy with region members removed.
        if (auto* authored = node_mut_locked_(n.id))
            authored->transport_sensitive = n.transport_sensitive;
    }

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
            cg->live_dsp_telemetry.prepare(tcfg, infos);
            cg->live_dsp_telemetry.set_enabled(
                desired_live_dsp_telemetry_enabled_.load(std::memory_order_relaxed));
        }
    }

    compute_latencies_for_(*cg, cg->connections, prepared_plugin_meta_);

    // Build the canonical-executor routing for this snapshot when the topology
    // is eligible. The Gain bindings resolve to THIS snapshot's own gain atomics
    // (valid for cg's whole lifetime), so the embedded snapshot needs no
    // keepalive. Ineligible graphs leave routed.serial.valid false and use the walk.
    {
        // Serial routed snapshot (compact buffer layout). The resolver set that
        // reads THIS snapshot's own runtime/plugins and the cached plugin
        // metadata lives in build_routing_snapshot_locked_, shared with the
        // parallel path so the two never drift.
        cg->routed.serial.valid = build_routing_snapshot_locked_(
            *cg, /*parallel_safe=*/false, cg->routed.serial.plugin_ctx,
            cg->routed.serial.custom_ctx, cg->routed.serial.snapshot);
        // Size THIS snapshot's own scratch pool (per-snapshot, retired with the
        // snapshot via RCU — never resized under an in-flight reader).
        if (cg->routed.serial.valid && max_block_size > 0) {
            cg->routed.serial.valid = cg->routed.serial.pool.reset(
                cg->routed.serial.snapshot.buffer_slot_count(),
                static_cast<std::uint32_t>(max_block_size),
                cg->routed.serial.snapshot.buffer_assignment().connection_delay_samples);
        }
        // Per-snapshot MIDI scratch + the MidiInput/MidiOutput node index lists
        // the routed dispatch bridges to the mailboxes. Built only when the
        // routed plan carries MIDI nodes, so audio-only graphs allocate none.
        cg->routed.midi_inputs.clear();
        cg->routed.midi_outputs.clear();
        cg->routed.processor_parameter_inputs.clear();
        if (cg->routed.serial.valid) {
            const auto& plan = cg->routed.serial.snapshot.plan();
            bool plan_has_midi = false;
            for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
                const auto kind = plan.nodes[i].kind;
                if (kind == graph::GraphRuntimeNodeKind::MidiInput) {
                    cg->routed.midi_inputs.push_back({i, plan.nodes[i].id});
                    plan_has_midi = true;
                } else if (kind == graph::GraphRuntimeNodeKind::MidiOutput) {
                    cg->routed.midi_outputs.push_back({i, plan.nodes[i].id});
                    plan_has_midi = true;
                } else if (plan.nodes[i].event_input_ports > 0 ||
                           plan.nodes[i].event_output_ports > 0) {
                    plan_has_midi = true; // a plugin carrying MIDI edges
                }
            }
            if (plan_has_midi) {
                cg->routed.serial.valid = cg->routed.midi.reset(plan.node_count());
            }
            // Per-snapshot sparse-automation scratch, built only when the routed
            // plan carries automation connections (audio-only / MIDI graphs
            // allocate none).
            bool plan_has_automation = false;
            for (const auto& conn : plan.connections) {
                if (graph::is_automation_conn(conn)) {
                    plan_has_automation = true;
                    break;
                }
            }
            if (cg->routed.serial.valid) {
                for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
                    const auto id = plan.nodes[i].id;
                    if (!processor_nodes_.contains(id))
                        continue;
                    const auto claim = exact_parameter_event_claims_.find(id);
                    if (claim == exact_parameter_event_claims_.end() || claim->second.expired())
                        continue;
                    cg->routed.processor_parameter_inputs.push_back({i, id, 0});
                    plan_has_automation = true;
                }
            }
            if (cg->routed.serial.valid && plan_has_automation && max_block_size > 0) {
                cg->routed.serial.valid =
                    cg->routed.automation.reset(plan, cg->routed.serial.snapshot.bindings(),
                                                static_cast<std::uint32_t>(max_block_size));
            }
        }

        // Levelized parallel routing: when enabled (at this prepare), build a
        // PARALLEL-SAFE (reuse-free) snapshot of the same eligible graph + its
        // levelization + a dedicated scratch pool, and ensure the persistent
        // worker pool is running. The MIDI/automation scratch and MidiInput/Output
        // node lists are SHARED with the serial path (identical plan). On any
        // failure the parallel path stays invalid and process() falls back.
        cg->routed.parallel.valid = false;
        if (cg->routed.serial.valid && parallel_routing_enabled_.load(std::memory_order_relaxed) &&
            max_block_size > 0) {
            // Parallel-safe routed snapshot: same resolver set as the serial
            // path (via build_routing_snapshot_locked_) but a reuse-free buffer
            // assignment so concurrent same-level nodes never alias a recycled
            // slot. The MIDI/automation scratch and MidiInput/Output node lists
            // are SHARED with the serial path (identical plan).
            bool ok = build_routing_snapshot_locked_(
                *cg, /*parallel_safe=*/true, cg->routed.parallel.plugin_ctx,
                cg->routed.parallel.custom_ctx, cg->routed.parallel.snapshot);
            if (ok) {
                cg->routed.parallel.levelization =
                    graph::build_graph_runtime_levelization(cg->routed.parallel.snapshot.plan());
                ok = cg->routed.parallel.levelization.ok &&
                     cg->routed.parallel.pool.reset(
                         cg->routed.parallel.snapshot.buffer_slot_count(),
                         static_cast<std::uint32_t>(max_block_size),
                         cg->routed.parallel.snapshot.buffer_assignment().connection_delay_samples);
            }
            if (ok && prepared_edit_origin_ == nullptr) {
                // Start the persistent worker pool off the audio thread, ONCE.
                // Hardware concurrency capped to a sane bound; participant 0 is
                // the audio thread, so the pool spawns worker_count - 1 threads.
                //
                // INVARIANT (load-bearing for audio-thread safety): the pool size
                // is fixed for the SignalGraph's lifetime and the pool is never
                // stopped/resized on a re-prepare. start()/stop() join worker
                // threads and reset epoch_/completed_/worker_count_; running them
                // concurrently with an in-flight process_parallel -> run() on the
                // audio thread would be a use-after-free. The only legal stop is
                // ~GraphRuntimeWorkerPool during ~SignalGraph, after the audio
                // thread has (by contract) stopped calling process(). So: start
                // only when not yet started (worker_count() == 0 — also retries a
                // previously failed start, safe because routed.parallel.valid was
                // false so process() never entered the parallel branch). A re-
                // prepare just re-checks running(); it must not restart the pool.
                if (worker_pool_.worker_count() == 0) {
                    const unsigned hw = std::thread::hardware_concurrency();
                    const std::uint32_t workers =
                        std::clamp<std::uint32_t>(hw == 0 ? 2 : hw, 2, 16);
                    ok = worker_pool_.start(workers);
                } else {
                    ok = worker_pool_.running();
                }
            }
            cg->routed.parallel.valid = ok;
        }

        const bool has_pdc =
            std::any_of(cg->connection_delays.begin(), cg->connection_delays.end(),
                        [](const ConnectionDelay& delay) { return delay.delay_samples > 0; });
        if (has_pdc) {
            if (cg->routed.parallel.valid &&
                parallel_routing_enabled_.load(std::memory_order_relaxed)) {
                cg->pdc_execution_domain = PdcExecutionDomain::RoutedParallel;
            } else if (cg->routed.serial.valid &&
                       canonical_executor_routing_enabled_.load(std::memory_order_relaxed)) {
                cg->pdc_execution_domain = PdcExecutionDomain::RoutedSerial;
            } else {
                cg->pdc_execution_domain = PdcExecutionDomain::Legacy;
            }
        }

        // Anticipative rendering: carve an eligible latent interior out of the
        // graph into a lane pre-rendered ahead of the deadline, and prepare the
        // live-path splice (a skip mask over the routed plan + a map from each lane
        // output channel to the interior boundary-source output slot it fills).
        // Requires the canonical routed snapshot (the splice runs on that path).
        // Recomputed every compile: with no active anticipation no node is forced
        // exterior, so the counter must read 0 rather than keep a prior value.
        transport_suppressed_for_anticipation_.store(0, std::memory_order_relaxed);
        if (mode == CompileMode::Normal && cg->routed.serial.valid && cg->processors.empty() &&
            anticipation_enabled_.load(std::memory_order_relaxed) && max_block_size > 0) {
            std::vector<NodeId> exact_parameter_input_nodes;
            exact_parameter_input_nodes.reserve(exact_parameter_event_claims_.size());
            for (const auto& [id, owner] : exact_parameter_event_claims_) {
                if (!owner.expired())
                    exact_parameter_input_nodes.push_back(id);
            }
            const auto eligibility = analyze_anticipation_eligibility(
                cg->executable_nodes, cg->connections, exact_parameter_input_nodes);
            // Record how many transport-sensitive nodes anticipation forced
            // exterior: each such node was seeded TransportSensitive above and so
            // is excluded from the interior, running live to observe the host
            // transport. This is the repurposed meaning of
            // transport_suppressed_for_anticipation() — no longer "transport
            // dropped per block" (transport now stays live; the masked interior
            // is transport-insensitive by construction).
            std::uint64_t transport_forced_exterior = 0;
            for (const auto& n : cg->executable_nodes) {
                if (n.transport_sensitive)
                    ++transport_forced_exterior;
            }
            transport_suppressed_for_anticipation_.store(transport_forced_exterior,
                                                         std::memory_order_relaxed);
            const auto partition =
                build_anticipation_partition(cg->executable_nodes, cg->connections, eligibility);
            const auto subgraph =
                build_anticipation_subgraph(cg->executable_nodes, cg->connections, partition);
            if (subgraph.renders_anything()) {
                CompiledGraph& cgr = *cg;
                constexpr int kLeadBlocks = 4;
                const bool prepared = cg->anticipation.lane.prepare(
                    subgraph,
                    [&cgr](NodeId id) -> std::atomic<float>* {
                        auto it = cgr.runtime.find(id);
                        return it == cgr.runtime.end() ? nullptr : it->second.gain.get();
                    },
                    [&cgr](NodeId id) -> PluginSlot* {
                        auto it = cgr.plugins.find(id);
                        return it == cgr.plugins.end() ? nullptr : it->second.get();
                    },
                    sample_rate, max_block_size, kLeadBlocks,
                    [&cgr](NodeId id) -> ParameterEventInjectionBinding {
                        auto it = cgr.runtime.find(id);
                        if (it == cgr.runtime.end() || !it->second.parameter_input_mailbox) {
                            return {};
                        }
                        return {
                            .user_data = &it->second,
                            .append = &SignalGraph::append_parameter_mailbox_events_,
                            .live_sequence_seen =
                                &it->second.parameter_input_mailbox->sequence_seen,
                            .exact_sequence_seen =
                                it->second.exact_parameter_input_mailbox
                                    ? &it->second.exact_parameter_input_mailbox->sequence_seen
                                    : nullptr,
                        };
                    });
                if (prepared) {
                    // Map each interior node id -> its dense index in the ROUTED
                    // plan, to build the skip mask and resolve boundary output slots.
                    const auto& rplan = cg->routed.serial.snapshot.plan();
                    const auto& rassign = cg->routed.serial.snapshot.buffer_assignment();
                    auto routed_index = [&rplan](NodeId id) -> std::uint32_t {
                        for (std::uint32_t i = 0; i < rplan.nodes.size(); ++i) {
                            if (rplan.nodes[i].id == id)
                                return i;
                        }
                        return 0xFFFFFFFFu;
                    };
                    cg->anticipation.skip_mask.assign(rplan.nodes.size(), 0);
                    bool map_ok = true;
                    for (const auto idx : partition.interior_nodes) {
                        const std::uint32_t ri = routed_index(cg->executable_nodes[idx].id);
                        if (ri == 0xFFFFFFFFu) {
                            map_ok = false;
                            break;
                        }
                        cg->anticipation.skip_mask[ri] = 1;
                    }
                    cg->anticipation.prefill.clear();
                    for (std::uint32_t ch = 0; map_ok && ch < subgraph.outputs.size(); ++ch) {
                        const auto& out = subgraph.outputs[ch];
                        const std::uint32_t ri = routed_index(out.source_node);
                        if (ri == 0xFFFFFFFFu) {
                            map_ok = false;
                            break;
                        }
                        const std::uint32_t slot = rassign.nodes[ri].output_base + out.source_port;
                        cg->anticipation.prefill.push_back({ch, slot});
                    }
                    if (map_ok) {
                        cg->anticipation.consume_scratch.assign(
                            subgraph.outputs.size(),
                            std::vector<float>(static_cast<std::size_t>(max_block_size), 0.0f));
                        cg->anticipation.consume_ptrs.clear();
                        for (auto& c : cg->anticipation.consume_scratch) {
                            cg->anticipation.consume_ptrs.push_back(c.data());
                        }
                        cg->anticipation.valid = true;
                    }
                }
            }
        }
    }
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
        auto read_guard = live_slot_.read();
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
            if (!n.plugin->prepare(sample_rate, max_block_size)) {
                runtime::log_error("SignalGraph: failed to prepare plugin '{}'", n.name);
                return false;
            }
            prepared_plugin_meta_[n.id] = PreparedPluginMetadata{
                n.plugin->parameters(), std::max(0, n.plugin->latency_samples()),
                n.plugin->wants_transport()};
        }
    }

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
    live_slot_.publish(std::move(cg));
    return true;
}

void SignalGraph::set_limits(GraphLimits limits) {
    // Mutates limits_ (read by validate_generated_graph under the lock) and drives
    // invalidate_live_locked_(); serialize against a concurrent prepare()/mutator.
    GraphMutationLock mutation_lock(*this);
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
