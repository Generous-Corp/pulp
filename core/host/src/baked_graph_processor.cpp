#include "baked_graph_processor_detail.hpp"
#include <cmath>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/sample_region_runtime.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::host {

namespace detail {

// Scalar state and callbacks belong to this baked instance, never its source.
struct BakedSampleRegionRuntime {
    std::vector<SampleRegionDefinition> definitions;
    std::vector<SampleKernelDescriptor> kernels;
    std::shared_ptr<SampleRegionStateBank> bank;
    std::unordered_map<NodeId, CustomNodeProcessFn> processes;
    std::vector<GraphNode> nodes;
    std::vector<Connection> connections;

    bool prepare(std::span<const GraphNode> authored_nodes,
                 std::span<const Connection> authored_connections,
                 const SampleRegionParameterBinding* parameters, double sample_rate,
                 int max_block) {
        processes.clear();
        bank.reset();
        nodes.clear();
        connections.clear();
        if (!parameters || !std::isfinite(sample_rate) || sample_rate <= 0.0 || max_block <= 0)
            return false;
        std::vector<SampleRegionCandidate> candidates;
        std::unordered_map<NodeId, const SampleRegionDefinition*> membership;
        for (const auto& definition : definitions) {
            if (definition.input_boundaries.empty() || definition.output_boundaries.empty())
                return false;
            const auto valid_boundaries = [&](const std::vector<NodeId>& boundaries,
                                              std::string_view type_id) {
                std::size_t count = 0;
                for (const auto& member : definition.members) {
                    if (member.type_id != type_id)
                        continue;
                    ++count;
                    const auto index = member.config.boundary_index_or_parameter_id;
                    if (member.config.kind != SampleKernelConfigKind::BoundaryIndex ||
                        index >= boundaries.size() || boundaries[index] != member.node)
                        return false;
                }
                return count == boundaries.size();
            };
            if (!valid_boundaries(definition.input_boundaries, "pulp.core.sample-region.input") ||
                !valid_boundaries(definition.output_boundaries, "pulp.core.sample-region.output"))
                return false;
            SampleRegionCandidate candidate;
            candidate.region_id = definition.region_id;
            candidate.members = definition.members;
            candidate.limits = definition.limits;
            candidate.max_block_size = static_cast<std::uint32_t>(max_block);
            candidate.registry = {
                this,
                [](const void* context, std::string_view id,
                   int version) noexcept -> const SampleKernelDescriptor* {
                    const auto& registry =
                        static_cast<const BakedSampleRegionRuntime*>(context)->kernels;
                    for (const auto& kernel : registry)
                        if (kernel.type_id == id && kernel.version == version)
                            return &kernel;
                    return nullptr;
                }};
            for (const auto& member : definition.members) {
                if (!membership.emplace(member.node, &definition).second)
                    return false;
                const auto found =
                    std::find_if(authored_nodes.begin(), authored_nodes.end(),
                                 [&](const auto& node) { return node.id == member.node; });
                if (found == authored_nodes.end() || found->type != NodeType::Custom ||
                    found->custom_type_id != member.type_id ||
                    found->custom_type_version != member.version)
                    return false;
            }
            for (const auto& parameter : definition.promoted_parameters)
                candidate.promoted_parameters.push_back(parameter.param_id);
            for (const auto& connection : authored_connections) {
                const auto touches = [&](NodeId id) {
                    return std::any_of(definition.members.begin(), definition.members.end(),
                                       [&](const auto& member) { return member.node == id; });
                };
                if (!touches(connection.source_node) && !touches(connection.dest_node))
                    continue;
                if (connection.midi || connection.automation || connection.audio_rate_modulation ||
                    connection.sidechain)
                    return false;
                candidate.connections.push_back({connection.source_node, connection.source_port,
                                                 connection.dest_node, connection.dest_port,
                                                 SampleRegionConnectionLane::PlainAudio,
                                                 connection.feedback});
            }
            candidates.push_back(std::move(candidate));
        }
        if (!prove_sample_regions(candidates).accepted)
            return false;
        std::vector<PreparedSampleRegionPlan> plans;
        for (const auto& candidate : candidates) {
            auto prepared = build_sample_region_plan(candidate);
            if (!prepared.proof.accepted || !prepared.plan)
                return false;
            plans.push_back(std::move(*prepared.plan));
        }
        auto fresh = SampleRegionStateBank::create_fresh(plans, sample_rate,
                                                         static_cast<std::uint32_t>(max_block), 1);
        if (!fresh)
            return false;
        for (std::size_t i = 0; i < plans.size(); ++i) {
            auto region = PreparedSampleRegion::create(std::move(plans[i]), fresh, parameters);
            if (!region)
                return false;
            processes.emplace(definitions[i].input_boundaries.front(),
                              [region = std::move(region)](
                                  audio::BufferView<float>& output,
                                  const audio::BufferView<const float>& input,
                                  int frames) { region->process(output, input, frames); });
        }
        for (const auto& authored : authored_nodes) {
            const auto member = membership.find(authored.id);
            if (member == membership.end()) {
                nodes.push_back(authored);
            } else if (authored.id == member->second->input_boundaries.front()) {
                auto anchor = authored;
                anchor.num_input_ports = static_cast<int>(member->second->input_boundaries.size());
                anchor.num_output_ports =
                    static_cast<int>(member->second->output_boundaries.size());
                anchor.custom_instance.reset();
                nodes.push_back(std::move(anchor));
            }
        }
        const auto remap = [](NodeId& id, PortIndex& port, const SampleRegionDefinition& definition,
                              bool input) {
            const auto& boundaries =
                input ? definition.input_boundaries : definition.output_boundaries;
            if (std::find(boundaries.begin(), boundaries.end(), id) == boundaries.end())
                return false;
            const auto member = std::find_if(definition.members.begin(), definition.members.end(),
                                             [&](const auto& value) { return value.node == id; });
            if (member == definition.members.end() ||
                member->config.kind != SampleKernelConfigKind::BoundaryIndex)
                return false;
            port = member->config.boundary_index_or_parameter_id;
            id = definition.input_boundaries.front();
            return true;
        };
        for (auto connection : authored_connections) {
            const auto source = membership.find(connection.source_node);
            const auto destination = membership.find(connection.dest_node);
            if (source != membership.end() && destination != membership.end() &&
                source->second == destination->second)
                continue;
            if (source != membership.end() &&
                !remap(connection.source_node, connection.source_port, *source->second, false))
                return false;
            if (destination != membership.end() &&
                !remap(connection.dest_node, connection.dest_port, *destination->second, true))
                return false;
            connections.push_back(std::move(connection));
        }
        bank = std::move(fresh);
        return true;
    }
};

} // namespace detail

namespace {
namespace fmt = pulp::format;

// The plugin's bus arity, derived from the AudioInput/AudioOutput node ports. Shared
// by bake() (in-memory) and bake_to_plan() (on-disk) so the derivation can't drift.
std::pair<int, int> derive_bus_arity(const SignalGraph& graph) {
    int input_channels = 0;
    int output_channels = 0;
    for (const auto& n : graph.nodes()) {
        if (n.type == NodeType::AudioInput) {
            input_channels = std::max(input_channels, n.num_output_ports);
        } else if (n.type == NodeType::AudioOutput) {
            output_channels = std::max(output_channels, n.num_input_ports);
        }
    }
    return {input_channels, output_channels};
}

LowerabilityProof prepared_custom_bindings_of(const SignalGraph& graph) {
    LowerabilityProof proof;
    for (const auto& node : graph.nodes()) {
        if (node.type != NodeType::Custom) continue;
        if (graph.live_custom_processor(node.id)
            || graph.live_custom_param_processor(node.id)) {
            continue;
        }
        proof.reason = LowerRejectReason::CustomNotYetLowerable;
        proof.offending_node = node.id;
        proof.message =
            "Custom node has no runnable prepared process binding; refusing to bake";
        return proof;
    }
    proof.accepted = true;
    proof.reason = LowerRejectReason::None;
    return proof;
}

LowerabilityProof
prepared_custom_bindings_of(const SignalGraph& graph,
                            const std::unordered_set<NodeId>& sample_region_nodes) {
    LowerabilityProof proof;
    for (const auto& node : graph.nodes()) {
        if (node.type != NodeType::Custom || sample_region_nodes.contains(node.id))
            continue;
        if (graph.live_custom_processor(node.id) || graph.live_custom_param_processor(node.id))
            continue;
        proof.reason = LowerRejectReason::CustomNotYetLowerable;
        proof.offending_node = node.id;
        proof.message = "Custom node has no runnable prepared process binding; refusing to bake";
        return proof;
    }
    proof.accepted = true;
    proof.reason = LowerRejectReason::None;
    return proof;
}
} // namespace

LowerabilityProof lowerability_of(
    std::span<const GraphNode> nodes,
    std::span<const Connection> connections,
    const std::function<const CustomNodeType*(std::string_view, int)>& resolve_custom) {
    LowerabilityProof proof;
    // Order matters: the Plugin/Custom node-kind refusals are checked BEFORE the
    // executor-eligibility predicate. A Plugin node with no live slot is executor-
    // ineligible; a Custom node now IS executor-eligible (it routes), but neither
    // can be baked — a hosted plugin and a custom instance both hold opaque state a
    // frozen topology cannot capture. The explicit kind checks give those graphs a
    // specific, actionable reason instead of a generic NotExecutorEligible.
    for (const auto& node : nodes) {
        if (node.type == NodeType::Plugin) {
            // A hosted Plugin owns opaque external state (its own DSP, presets,
            // sample caches). Freezing the topology cannot capture that state, so
            // a baked Processor would not be self-contained.
            proof.reason = LowerRejectReason::HostedPluginNotSelfContained;
            proof.offending_node = node.id;
            proof.message =
                "hosted Plugin node holds opaque external state and is not "
                "self-contained; refusing to bake";
            return proof;
        }
        if (node.type == NodeType::Custom) {
            // A Custom node lowers only if its registered type opted in (lowerable),
            // matches the node's shape, and is transport-independent (the baked
            // process() drops the host transport). Without a resolver the type is
            // unknown, so any Custom node is refused.
            const CustomNodeType* type =
                resolve_custom
                    ? resolve_custom(node.custom_type_id, node.custom_type_version)
                    : nullptr;
            if (type == nullptr) {
                proof.reason = LowerRejectReason::CustomNotYetLowerable;
                proof.offending_node = node.id;
                proof.message =
                    resolve_custom
                        ? "Custom node type is not registered/resolvable; refusing to bake"
                        : "Custom node lowering requires a registered type; refusing to bake";
                return proof;
            }
            if (node.num_input_ports != type->num_input_ports ||
                node.num_output_ports != type->num_output_ports) {
                proof.reason = LowerRejectReason::CustomNotYetLowerable;
                proof.offending_node = node.id;
                proof.message =
                    "Custom node shape does not match its registered type; refusing to bake";
                return proof;
            }
            if (!type->lowerable) {
                proof.reason = LowerRejectReason::CustomNotLowerable;
                proof.offending_node = node.id;
                proof.message =
                    "Custom node type is not opted into baking (lowerable=false); "
                    "refusing to bake";
                return proof;
            }
            if (type->process_transport || type->process_instance_transport) {
                proof.reason = LowerRejectReason::CustomTransportNotLowerable;
                proof.offending_node = node.id;
                proof.message =
                    "transport-sensitive Custom node is not lowerable (the baked "
                    "process drops the host transport); refusing to bake";
                return proof;
            }
            // Accepted — a lowerable, shape-matched, transport-independent Custom
            // node. Skip the audio-only kind check below.
            continue;
        }
        // The lowerable subset is audio-only. The routed executor also accepts
        // MidiInput/MidiOutput nodes, but a BakedGraphProcessor advertises no MIDI
        // bus and process() carries no MIDI scratch, so a MIDI node would be
        // silently dropped — refuse rather than bake a graph that cannot match.
        if (node.type != NodeType::AudioInput && node.type != NodeType::AudioOutput &&
            node.type != NodeType::Gain) {
            proof.reason = LowerRejectReason::NonAudioLaneNotLowerable;
            proof.offending_node = node.id;
            proof.message =
                "only audio I/O and Gain nodes are lowerable in this slice; MIDI and "
                "other node kinds are a follow-up";
            return proof;
        }
    }

    // Likewise refuse any non-audio connection lane. The executor can route MIDI /
    // automation / audio-rate-modulation / sidechain edges, but this slice bakes
    // only plain audio, and process() supplies no MIDI/automation scratch — so such
    // an edge would diverge from the live graph. Fail closed.
    for (const auto& c : connections) {
        if (c.midi || c.automation || c.audio_rate_modulation || c.sidechain) {
            proof.reason = LowerRejectReason::NonAudioLaneNotLowerable;
            proof.offending_node = c.dest_node;
            proof.message =
                "only plain audio connections are lowerable in this slice; "
                "MIDI/automation/audio-rate-modulation/sidechain are a follow-up";
            return proof;
        }
    }

    if (!validate_signal_graph_executor_topology(nodes, connections)) {
        proof.reason = LowerRejectReason::NotExecutorEligible;
        proof.message =
            "graph is outside the routed executor's supported topology or resource "
            "limits; refusing to bake";
        return proof;
    }

    proof.accepted = true;
    proof.reason = LowerRejectReason::None;
    return proof;
}

using CustomStateRestoreMap =
    std::unordered_map<NodeId, std::vector<std::uint8_t>>;

static LowerResult bake_impl(const SignalGraph& graph,
                             const CustomStateRestoreMap* restore_states) {
    LowerResult result;

    // Lowerability gate. Order matters: the Plugin/Custom node-kind refusals are
    // checked BEFORE the executor-eligibility predicate. A Plugin node with no
    // live slot is executor-ineligible; a Custom node now IS executor-eligible
    // (it routes), but neither can be baked — a hosted plugin and a custom
    // instance both hold opaque state a frozen topology cannot capture. The
    // explicit kind checks give those graphs a specific, actionable reason
    // instead of a generic NotExecutorEligible.
    if (!graph.is_prepared()) {
        result.reason = LowerRejectReason::NotPrepared;
        result.message = "graph is not prepared; call prepare() before bake()";
        return result;
    }

    std::unordered_set<NodeId> region_members;
    std::vector<SampleKernelDescriptor> sample_kernels;
    for (const auto& region : graph.sample_regions()) {
        for (const auto& member : region.members) {
            region_members.insert(member.node);
            const auto* kernel = graph.sample_kernel_type(member.type_id, member.version);
            if (!kernel) {
                result.reason = LowerRejectReason::CustomNotYetLowerable;
                result.offending_node = member.node;
                result.message = "sample region requires an exact scalar kernel";
                return result;
            }
            if (std::none_of(sample_kernels.begin(), sample_kernels.end(), [&](const auto& value) {
                    return value.type_id == kernel->type_id && value.version == kernel->version;
                }))
                sample_kernels.push_back(*kernel);
        }
    }
    if (!region_members.empty()) {
        const auto proof = bake_to_plan(graph);
        if (!proof.accepted) {
            result.reason = proof.reason;
            result.offending_node = proof.offending_node;
            result.message = proof.message;
            return result;
        }
    } else {
        // Topology lowerability — the shared gate (see lowerability_of). bake()'s only
        // extra precondition is is_prepared() above; the node-kind / lane / executor-
        // eligibility proof is identical to what the on-disk load path will re-run.
        if (const auto proof = lowerability_of(graph.nodes(), graph.connections(),
                                               [&graph](std::string_view type_id, int version) {
                                                   return graph.custom_node_type(type_id, version);
                                               });
            !proof.accepted) {
            result.reason = proof.reason;
            result.offending_node = proof.offending_node;
            result.message = proof.message;
            return result;
        }
        if (const auto proof = prepared_custom_bindings_of(graph); !proof.accepted) {
            result.reason = proof.reason;
            result.offending_node = proof.offending_node;
            result.message = proof.message;
            return result;
        }
    }

    // Capture owned node specs, Gain values, connections, and bus arity for the
    // immutable baked plan.
    std::vector<GraphNode> nodes;
    nodes.reserve(graph.nodes().size());
    // For each lowerable Custom node, capture a COPY of the live resolved process
    // callback — it captured the custom instance shared_ptr by value, so the copy
    // carries the instance keepalive into the baked Processor (self-contained, no
    // reference back into the source graph). For a STATEFUL node (live instance),
    // also capture the type's prepare/reset hooks bound to that instance, so the
    // baked Processor's prepare() can re-init the instance's DSP state at the
    // host's real rate/block — otherwise stale state (e.g. a delay line's
    // contents) would survive into the baked stream, breaking the fresh-stream
    // contract documented in the header.
    // One record per lowerable Custom node: process callback, intrinsic latency,
    // lifecycle hooks, and any bake-layer param-injection binding. The bound
    // callbacks captured the instance keepalive (self-contained), and declared
    // params come from the registered type — never from the graph node.
    std::unordered_map<NodeId, BakedCustomNodeBinding> custom_nodes;
    const auto [input_channels, output_channels] = derive_bus_arity(graph);
    for (const auto& src : graph.nodes()) {
        GraphNode n;
        n.id = src.id;
        n.type = src.type;
        n.name = src.name;
        n.num_input_ports = src.num_input_ports;
        n.num_output_ports = src.num_output_ports;
        if (src.type == NodeType::Gain) {
            n.gain = graph.node_gain(src.id);
        } else if (src.type == NodeType::Custom) {
            n.custom_type_id = src.custom_type_id;
            n.custom_type_version = src.custom_type_version;
            if (region_members.contains(src.id)) {
                nodes.push_back(std::move(n));
                continue;
            }
            const CustomNodeType* type = graph.custom_node_type(
                src.custom_type_id, src.custom_type_version);
            if (const CustomNodeProcessFn* fn = graph.live_custom_processor(src.id)) {
                custom_nodes[src.id].process = *fn;
            }
            if (src.custom_instance) {
                const std::vector<std::uint8_t>* restore_state = nullptr;
                if (restore_states) {
                    const auto restore_it = restore_states->find(src.id);
                    if (restore_it != restore_states->end())
                        restore_state = &restore_it->second;
                }
                if (type != nullptr &&
                    (type->prepare || type->reset || restore_state)) {
                    CustomNodeLifecycle lc;
                    auto inst = src.custom_instance;  // shared keepalive per closure
                    if (type->prepare) {
                        lc.prepare = [inst, fn = type->prepare](double sample_rate,
                                                                int max_block) {
                            fn(inst.get(), sample_rate, max_block);
                        };
                    }
                    if (type->reset) {
                        lc.reset = [inst, fn = type->reset]() { fn(inst.get()); };
                    }
                    if (restore_state && type->load_state) {
                        auto state = *restore_state;
                        lc.restore_state =
                            [inst, fn = type->load_state,
                             state = std::move(state)]() {
                                return fn(inst.get(), state);
                            };
                    }
                    custom_nodes[src.id].lifecycle = std::move(lc);
                }
            }
            if (const CustomNodeParamProcessFn* pfn =
                    graph.live_custom_param_processor(src.id)) {
                if (type) {
                    custom_nodes[src.id].params =
                        BakedCustomParamBinding{*pfn, type->baked_params};
                }
            }
            // Mirror the live rule: attach latency only when a plain or
            // baked-param callback will actually execute. A registered type
            // with neither is transparent here, so it must not add latency.
            // Evaluated at the graph's own rate and clamped into the declared
            // range, exactly as the live compile path does.
            if (type && type->latency_samples) {
                if (auto it = custom_nodes.find(src.id);
                    it != custom_nodes.end()
                    && (it->second.process || it->second.params.process)) {
                    it->second.latency_samples = type->latency_samples;
                }
            }
        }
        nodes.push_back(std::move(n));
    }
    std::vector<Connection> conns(graph.connections().begin(), graph.connections().end());
    std::vector<SampleRegionDefinition> sample_regions;
    for (auto& descriptor : graph.sample_regions()) {
        SampleRegionDefinition definition;
        static_cast<SampleRegionDefinition&>(definition) = std::move(descriptor);
        sample_regions.push_back(std::move(definition));
    }

    return BakedGraphProcessor::create_with_sample_regions(
        std::move(nodes), std::move(conns), input_channels > 0 ? input_channels : 2,
        output_channels > 0 ? output_channels : 2, "Baked Graph", "com.pulp.baked-graph",
        std::move(custom_nodes), std::move(sample_regions), std::move(sample_kernels));
}

LowerResult bake(const SignalGraph& graph) {
    return bake_impl(graph, nullptr);
}

BakePlanResult bake_to_plan(const SignalGraph& graph) {
    BakePlanResult result;
    if (!graph.is_prepared()) {
        result.reason = LowerRejectReason::NotPrepared;
        result.message = "graph is not prepared; call prepare() before bake_to_plan()";
        return result;
    }
    const auto authored_regions = graph.sample_regions();
    std::unordered_set<NodeId> sample_region_nodes;
    for (const auto& region : authored_regions) {
        const auto proof = graph.prove_sample_region(region.region_id);
        if (!proof.accepted) {
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_node = proof.offending_node;
            result.message =
                proof.message.empty() ? "sample-region proof rejected graph" : proof.message;
            return result;
        }
        for (const auto& member : region.members)
            sample_region_nodes.insert(member.node);
    }

    std::vector<GraphNode> proof_nodes;
    std::vector<Connection> proof_connections;
    if (sample_region_nodes.empty()) {
        proof_nodes.assign(graph.nodes().begin(), graph.nodes().end());
        proof_connections.assign(graph.connections().begin(), graph.connections().end());
    } else {
        for (const auto& node : graph.nodes()) {
            if (!sample_region_nodes.contains(node.id))
                proof_nodes.push_back(node);
        }
        for (const auto& connection : graph.connections()) {
            if (!sample_region_nodes.contains(connection.source_node) &&
                !sample_region_nodes.contains(connection.dest_node))
                proof_connections.push_back(connection);
        }
    }
    if (const auto proof = lowerability_of(proof_nodes, proof_connections,
                                           [&graph](std::string_view type_id, int version) {
                                               return graph.custom_node_type(type_id, version);
                                           });
        !proof.accepted) {
        result.reason = proof.reason;
        result.offending_node = proof.offending_node;
        result.message = proof.message;
        return result;
    }
    const auto custom_bindings = sample_region_nodes.empty()
                                     ? prepared_custom_bindings_of(graph)
                                     : prepared_custom_bindings_of(graph, sample_region_nodes);
    if (!custom_bindings.accepted) {
        const auto& proof = custom_bindings;
        result.reason = proof.reason;
        result.offending_node = proof.offending_node;
        result.message = proof.message;
        return result;
    }
    BakedPlan plan;
    // Region-free artifacts retain the v1 wire contract. Region-bearing bake
    // output is admitted by the v2 codec only after the runtime integration
    // owns the sample-region lowering; this slice still emits v1 for the
    // existing block-graph path.
    plan.format_version =
        authored_regions.empty() ? kBakedPlanV1FormatVersion : kBakedMaxSupportedFormatVersion;
    for (const auto& descriptor : authored_regions) {
        SampleRegionDefinition definition;
        static_cast<SampleRegionDefinition&>(definition) = descriptor;
        plan.sample_regions.push_back(std::move(definition));
    }
    const auto [input_channels, output_channels] = derive_bus_arity(graph);
    plan.input_channels = input_channels;
    plan.output_channels = output_channels;
    for (const auto& src : graph.nodes()) {
        BakedPlan::Node n;
        n.id = src.id;
        n.type = src.type;
        n.num_input_ports = src.num_input_ports;
        n.num_output_ports = src.num_output_ports;
        if (src.type == NodeType::Gain) {
            n.gain = graph.node_gain(src.id);
        } else if (src.type == NodeType::Custom) {
            n.custom_type_id = src.custom_type_id;
            n.custom_version = src.custom_type_version;
            // Persist the authored/staged blob, never the live instance's
            // save_state(): a prepared graph may be processing concurrently and
            // Custom save_state has no audio-thread concurrency contract.
            n.custom_state = src.custom_state_blob;
            if (n.custom_state.size() > kBakedMaxCustomState) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_node = src.id;
                result.message = "Custom node state exceeds the signed bake limit";
                return result;
            }
        }
        plan.nodes.push_back(std::move(n));
    }
    for (const auto& c : graph.connections()) {
        // Lowerability already refused MIDI/automation/sidechain lanes; guard anyway
        // so only audio + feedback edges reach the plan.
        if (c.midi || c.automation || c.sidechain || c.audio_rate_modulation) continue;
        BakedPlan::Conn cc;
        cc.src_node = c.source_node;
        cc.src_port = static_cast<int>(c.source_port);
        cc.dst_node = c.dest_node;
        cc.dst_port = static_cast<int>(c.dest_port);
        cc.feedback = c.feedback;
        plan.connections.push_back(cc);
    }
    result.plan = std::move(plan);
    result.accepted = true;
    return result;
}

LowerResult load_baked(std::span<const std::uint8_t> bytes, const BakedTrust& trust,
                       const std::vector<CustomNodeType>& custom_types) {
    LowerResult result;
    const auto plan = verify_and_extract_plan(bytes, trust);
    if (!plan) {
        result.reason = LowerRejectReason::CodecRejected;
        result.message = "signed .pulpbake envelope or bounded plan parse failed";
        return result;
    }
    // The legacy source signature is intentionally a v1-only path. A v2
    // artifact carries sample-region semantics that this block-only processor
    // cannot execute; accepting it here would silently flatten sample timing.
    if (plan->format_version != kBakedPlanV1FormatVersion) {
        result.reason = LowerRejectReason::CodecRejected;
        result.message =
            "signed .pulpbake v2 requires paired sample-kernel loading; refusing in v1 loader";
        return result;
    }

    // Rebuild the verified plan into a fresh SignalGraph and lower it through bake(),
    // so bake()'s full lowerability re-proof + custom resolution run on the
    // reconstructed topology. The file's implicit claim is never trusted.
    SignalGraph graph;
    for (std::size_t i = 0; i < custom_types.size(); ++i) {
        for (std::size_t j = i + 1; j < custom_types.size(); ++j) {
            if (custom_types[i].type_id == custom_types[j].type_id &&
                custom_types[i].version == custom_types[j].version) {
                result.reason = LowerRejectReason::CodecRejected;
                result.message =
                    "duplicate Custom type identity in load registry";
                return result;
            }
        }
        if (!graph.register_custom_node_type(custom_types[i])) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "invalid Custom type in load registry";
            return result;
        }
    }

    std::unordered_map<NodeId, NodeId> id_map;
    CustomStateRestoreMap restore_states;
    for (const auto& n : plan->nodes) {
        NodeId gid = 0;
        const CustomNodeType* resolved_type = nullptr;
        bool has_state_lifecycle = false;
        if (n.type == NodeType::Custom) {
            resolved_type =
                graph.custom_node_type(n.custom_type_id, n.custom_version);
            has_state_lifecycle =
                resolved_type && resolved_type->create &&
                resolved_type->load_state;
            if (!n.custom_state.empty() && !has_state_lifecycle) {
                result.reason = LowerRejectReason::CustomNotYetLowerable;
                result.offending_node = n.id;
                result.message =
                    "stateful Custom node requires a matching registered type "
                    "with create + load_state";
                return result;
            }
        }
        switch (n.type) {
            case NodeType::AudioInput:
                gid = graph.add_input_node(n.num_output_ports, "in");
                break;
            case NodeType::AudioOutput:
                gid = graph.add_output_node(n.num_input_ports, "out");
                break;
            case NodeType::Gain:
                gid = graph.add_gain_node("gain");
                if (gid != 0) graph.set_node_gain(gid, n.gain);
                break;
            case NodeType::Custom:
                gid = graph.add_custom_node(n.custom_type_id, n.custom_version, "custom");
                break;
            default:  // Plugin / MIDI — never lowerable; refuse loudly.
                result.reason = LowerRejectReason::NonAudioLaneNotLowerable;
                result.offending_node = n.id;
                return result;
        }
        if (gid == 0) {
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_node = n.id;
            result.message = "could not reconstruct a plan node";
            return result;
        }
        id_map[n.id] = gid;
        // A zero-byte blob can be meaningful opaque state. Presence is derived
        // from the authoritative registered lifecycle, never from blob length.
        if (n.type == NodeType::Custom && has_state_lifecycle) {
            if (!graph.set_custom_node_state(gid, n.custom_state)) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_node = n.id;
                result.message = "could not stage authenticated Custom node state";
                return result;
            }
            if (!restore_states.emplace(gid, n.custom_state).second) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_node = n.id;
                result.message = "duplicate reconstructed Custom state identity";
                return result;
            }
        }
    }
    for (const auto& c : plan->connections) {
        const auto s = id_map.find(c.src_node);
        const auto d = id_map.find(c.dst_node);
        if (s == id_map.end() || d == id_map.end()) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "connection references an unknown node";
            return result;
        }
        const bool ok = c.feedback
                            ? graph.connect_feedback(s->second, c.src_port, d->second, c.dst_port)
                            : graph.connect(s->second, c.src_port, d->second, c.dst_port);
        if (!ok) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "could not reconstruct a connection";
            return result;
        }
    }

    // Nominal prepare only to satisfy bake()'s prepared-graph precondition (bake reads
    // topology, not buffer sizing). The returned Processor re-prepares at the host's
    // real rate/block, so these values never reach the audio path — routing the plan
    // back through bake() is what re-proves lowerability on the reconstructed graph.
    constexpr double kNominalPrepareSampleRate = 48000.0;
    constexpr int kNominalPrepareBlock = 512;
    if (!graph.prepare(kNominalPrepareSampleRate, kNominalPrepareBlock)) {
        const NodeId failed_graph_node =
            graph.last_prepare_custom_failure_node();
        if (failed_graph_node != 0) {
            result.reason = LowerRejectReason::StatefulCustomNotYetLoadable;
            const auto failed_plan_node = std::find_if(
                id_map.begin(), id_map.end(),
                [failed_graph_node](const auto& ids) {
                    return ids.second == failed_graph_node;
                });
            if (failed_plan_node != id_map.end())
                result.offending_node = failed_plan_node->first;
            result.message =
                "registered Custom type could not create an instance or "
                "rejected authenticated state";
        } else {
            result.reason = LowerRejectReason::NotPrepared;
            result.message = "reconstructed graph failed to prepare";
        }
        return result;
    }
    return bake_impl(graph, &restore_states);
}

BakedPlanLoadResult load_baked_plan(std::span<const std::uint8_t> bytes, const BakedTrust& trust,
                                    const std::vector<BakedTypeRegistration>& registrations) {
    BakedPlanLoadResult result;
    const auto plan = verify_and_extract_plan(bytes, trust);
    if (!plan) {
        result.reason = LowerRejectReason::CodecRejected;
        result.message = "signed .pulpbake envelope or bounded plan parse failed";
        return result;
    }
    if (plan->format_version == kBakedPlanV1FormatVersion) {
        if (!plan->sample_regions.empty()) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "v1 baked plan unexpectedly contains sample-region metadata";
            return result;
        }
        std::vector<CustomNodeType> legacy_types;
        legacy_types.reserve(registrations.size());
        for (const auto& registration : registrations)
            legacy_types.push_back(registration.block);
        const auto legacy = load_baked(bytes, trust, legacy_types);
        if (!legacy.accepted) {
            result.reason = legacy.reason;
            result.offending_node = legacy.offending_node;
            result.message = legacy.message;
            return result;
        }
        result.plan = *plan;
        result.accepted = true;
        result.reason = LowerRejectReason::None;
        return result;
    }
    if (plan->format_version != kBakedMaxSupportedFormatVersion || plan->sample_regions.empty()) {
        result.reason = LowerRejectReason::CodecRejected;
        result.message = "paired sample-kernel loading requires a non-empty v2 region table";
        return result;
    }

    int derived_input_channels = 0;
    int derived_output_channels = 0;
    for (const auto& node : plan->nodes) {
        if (node.id == 0) {
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_node = node.id;
            result.message = "v2 baked plan node IDs must be nonzero";
            return result;
        }
        switch (node.type) {
        case NodeType::AudioInput:
            if (node.num_input_ports != 0 || node.num_output_ports <= 0 ||
                node.num_output_ports > kBakedMaxPortsPerNode) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_node = node.id;
                result.message = "v2 AudioInput shape is not canonical";
                return result;
            }
            derived_input_channels = std::max(derived_input_channels, node.num_output_ports);
            break;
        case NodeType::AudioOutput:
            if (node.num_input_ports <= 0 || node.num_input_ports > kBakedMaxPortsPerNode ||
                node.num_output_ports != 0) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_node = node.id;
                result.message = "v2 AudioOutput shape is not canonical";
                return result;
            }
            derived_output_channels = std::max(derived_output_channels, node.num_input_ports);
            break;
        case NodeType::Gain:
            if (node.num_input_ports != 2 || node.num_output_ports != 2) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_node = node.id;
                result.message = "v2 Gain shape is not canonical";
                return result;
            }
            break;
        case NodeType::Custom:
            if (node.num_input_ports < 0 || node.num_output_ports < 0 ||
                node.num_input_ports > kBakedMaxPortsPerNode ||
                node.num_output_ports > kBakedMaxPortsPerNode || node.custom_type_id.empty() ||
                node.custom_version <= 0) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_node = node.id;
                result.message = "v2 Custom shape or identity is not canonical";
                return result;
            }
            break;
        default:
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_node = node.id;
            result.message = "v2 baked plan contains a non-audio node";
            return result;
        }
    }
    if (plan->input_channels != derived_input_channels ||
        plan->output_channels != derived_output_channels) {
        result.reason = LowerRejectReason::CodecRejected;
        result.message = "v2 baked plan bus arity does not match its AudioInput/AudioOutput nodes";
        return result;
    }

    // Validate the pair identities before mutating the isolated candidate.
    std::unordered_set<std::string> registration_keys;
    for (const auto& registration : registrations) {
        if (!registration.block.is_valid_registration() ||
            !registration.sample.is_valid_registration() ||
            registration.block.type_id != registration.sample.type_id ||
            registration.block.version != registration.sample.version ||
            registration.block.num_input_ports < 0 || registration.block.num_output_ports < 0 ||
            registration.sample.num_input_ports !=
                static_cast<std::uint32_t>(registration.block.num_input_ports) ||
            registration.sample.num_output_ports !=
                static_cast<std::uint32_t>(registration.block.num_output_ports)) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "paired CustomNodeType/SampleKernelDescriptor identity mismatch";
            return result;
        }
        const auto key =
            registration.block.type_id + "\x1f" + std::to_string(registration.block.version);
        if (!registration_keys.insert(key).second) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "duplicate paired sample-kernel registration";
            return result;
        }
    }

    SignalGraph owner;
    auto edit = owner.begin_prepared_topology_edit();
    if (!edit || !register_builtin_sample_region_types(*edit)) {
        result.reason = LowerRejectReason::CodecRejected;
        result.message = "built-in sample-kernel registrar failed";
        return result;
    }
    for (const auto& registration : registrations) {
        if (!edit->register_custom_node_type(registration.block, registration.sample)) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "paired sample-kernel registration was refused";
            return result;
        }
    }

    std::unordered_map<NodeId, NodeId> id_map;
    id_map.reserve(plan->nodes.size());
    for (const auto& node : plan->nodes) {
        NodeId mapped = 0;
        switch (node.type) {
        case NodeType::AudioInput:
            mapped = edit->add_input_node(node.num_output_ports, "in");
            break;
        case NodeType::AudioOutput:
            mapped = edit->add_output_node(node.num_input_ports, "out");
            break;
        case NodeType::Gain:
            mapped = edit->add_gain_node("gain");
            if (mapped != 0 && !edit->set_node_gain(mapped, node.gain))
                mapped = 0;
            break;
        case NodeType::Custom:
            mapped = edit->add_custom_node(node.custom_type_id, node.custom_version, "custom");
            break;
        default:
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_node = node.id;
            result.message = "v2 baked plan contains a non-audio node";
            return result;
        }
        const auto* reconstructed = mapped == 0 ? nullptr : edit->node(mapped);
        if (mapped == 0 || reconstructed == nullptr ||
            reconstructed->num_input_ports != node.num_input_ports ||
            reconstructed->num_output_ports != node.num_output_ports ||
            !id_map.emplace(node.id, mapped).second) {
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_node = node.id;
            result.message = "could not reconstruct a unique v2 plan node";
            return result;
        }
        if (node.type == NodeType::Custom && !node.custom_state.empty()) {
            const CustomNodeType* block_type = nullptr;
            for (const auto& registration : registrations) {
                if (registration.block.type_id == node.custom_type_id &&
                    registration.block.version == node.custom_version) {
                    block_type = &registration.block;
                    break;
                }
            }
            if (block_type == nullptr || !block_type->create || !block_type->load_state) {
                result.reason = LowerRejectReason::StatefulCustomNotYetLoadable;
                result.offending_node = node.id;
                result.message =
                    "stateful v2 Custom node requires paired create + load_state callbacks";
                return result;
            }
        }
    }

    std::vector<SampleRegionDefinition> remapped_regions;
    remapped_regions.reserve(plan->sample_regions.size());
    std::unordered_map<NodeId, SampleRegionId> region_for_node;
    for (const auto& authored : plan->sample_regions) {
        auto region = authored;
        for (auto& member : region.members) {
            const auto mapped = id_map.find(member.node);
            if (mapped == id_map.end()) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_region = region.region_id;
                result.offending_node = member.node;
                result.message = "region member references an unknown plan node";
                return result;
            }
            member.node = mapped->second;
            if (!region_for_node.emplace(member.node, region.region_id).second) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_region = region.region_id;
                result.offending_node = member.node;
                result.message = "region member belongs to multiple authored regions";
                return result;
            }
        }
        for (auto& boundary : region.input_boundaries) {
            const auto mapped = id_map.find(boundary);
            if (mapped == id_map.end()) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_region = region.region_id;
                result.offending_node = boundary;
                result.message = "region input boundary references an unknown plan node";
                return result;
            }
            boundary = mapped->second;
        }
        for (auto& boundary : region.output_boundaries) {
            const auto mapped = id_map.find(boundary);
            if (mapped == id_map.end()) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_region = region.region_id;
                result.offending_node = boundary;
                result.message = "region output boundary references an unknown plan node";
                return result;
            }
            boundary = mapped->second;
        }
        for (auto& parameter : region.promoted_parameters) {
            const auto mapped = id_map.find(parameter.bound_node_id);
            if (mapped == id_map.end()) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_region = region.region_id;
                result.offending_node = parameter.bound_node_id;
                result.message = "promoted parameter references an unknown plan node";
                return result;
            }
            parameter.bound_node_id = mapped->second;
        }
        if (!edit->declare_sample_region(region).accepted) {
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_region = authored.region_id;
            result.message = "reconstructed region metadata failed authoring validation";
            return result;
        }
        remapped_regions.push_back(std::move(region));
    }

    // Region-internal edges use the region-aware insertion path because a legal
    // UnitDelay cycle is intentionally not an ordinary graph cycle. All other
    // edges retain the existing graph connection semantics.
    for (const auto& connection : plan->connections) {
        const auto source = id_map.find(connection.src_node);
        const auto destination = id_map.find(connection.dst_node);
        if (source == id_map.end() || destination == id_map.end()) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "v2 connection references an unknown node";
            return result;
        }
        const auto source_region = region_for_node.find(source->second);
        const auto destination_region = region_for_node.find(destination->second);
        const bool same_region = source_region != region_for_node.end() &&
                                 destination_region != region_for_node.end() &&
                                 source_region->second == destination_region->second;
        bool accepted = false;
        if (same_region) {
            if (connection.feedback) {
                result.reason = LowerRejectReason::CodecRejected;
                result.offending_region = source_region->second;
                result.message = "legacy feedback is not valid inside a sample region";
                return result;
            }
            accepted = edit->connect_in_sample_region(source_region->second, source->second,
                                                      connection.src_port, destination->second,
                                                      connection.dst_port)
                           .accepted;
        } else if (connection.feedback) {
            accepted = edit->connect_feedback(source->second, connection.src_port,
                                              destination->second, connection.dst_port);
        } else {
            accepted = edit->connect(source->second, connection.src_port, destination->second,
                                     connection.dst_port);
        }
        if (!accepted) {
            result.reason = LowerRejectReason::CodecRejected;
            result.message = "could not reconstruct a v2 connection";
            return result;
        }
    }

    for (const auto& region : remapped_regions) {
        const auto proof = edit->prove_sample_region(region.region_id);
        if (!proof.accepted) {
            result.reason = LowerRejectReason::CodecRejected;
            result.offending_region = region.region_id;
            result.offending_node = proof.offending_node;
            result.message =
                proof.message.empty() ? "sample-region proof rejected v2 artifact" : proof.message;
            return result;
        }
    }

    // The ordinary block proof applies to the residual graph only. Region
    // members are scalar kernels and must not be fed through the block bake
    // lowerability predicate.
    std::vector<GraphNode> residual_nodes;
    residual_nodes.reserve(edit->nodes().size());
    for (const auto& node : edit->nodes()) {
        if (!region_for_node.contains(node.id))
            residual_nodes.push_back(node);
    }
    std::vector<Connection> residual_connections;
    for (const auto& connection : edit->connections()) {
        if (!region_for_node.contains(connection.source_node) &&
            !region_for_node.contains(connection.dest_node)) {
            residual_connections.push_back(connection);
        }
    }
    const auto ordinary = lowerability_of(
        residual_nodes, residual_connections,
        [&registrations](std::string_view type_id, int version) -> const CustomNodeType* {
            for (const auto& registration : registrations) {
                if (registration.block.type_id == type_id && registration.block.version == version)
                    return &registration.block;
            }
            return nullptr;
        });
    if (!ordinary.accepted) {
        result.reason = ordinary.reason;
        result.offending_node = ordinary.offending_node;
        result.message = ordinary.message;
        return result;
    }

    result.plan = *plan;
    result.accepted = true;
    result.reason = LowerRejectReason::None;
    return result;
}

BakedGraphProcessor::BakedGraphProcessor(
    std::vector<GraphNode> nodes, std::vector<Connection> connections, int input_channels,
    int output_channels, std::string name, std::string bundle_id,
    std::unordered_map<NodeId, BakedCustomNodeBinding> custom_nodes)
    : BakedGraphProcessor(
          std::move(nodes), std::move(connections), input_channels, output_channels,
          std::move(name), std::move(bundle_id), std::move(custom_nodes),
          SampleRegionParameterContract::from_regions(std::vector<SampleRegionDefinition>{})
              .freeze(std::span<const state::ParamInfo>{})) {}

LowerResult BakedGraphProcessor::create_with_sample_regions(
    std::vector<GraphNode> nodes, std::vector<Connection> connections, int input_channels,
    int output_channels, std::string name, std::string bundle_id,
    std::unordered_map<NodeId, BakedCustomNodeBinding> custom_nodes,
    std::vector<SampleRegionDefinition> sample_regions) {
    return create_with_sample_regions(std::move(nodes), std::move(connections), input_channels,
                                      output_channels, std::move(name), std::move(bundle_id),
                                      std::move(custom_nodes), std::move(sample_regions), {});
}

LowerResult BakedGraphProcessor::create_with_sample_regions(
    std::vector<GraphNode> nodes, std::vector<Connection> connections, int input_channels,
    int output_channels, std::string name, std::string bundle_id,
    std::unordered_map<NodeId, BakedCustomNodeBinding> custom_nodes,
    std::vector<SampleRegionDefinition> sample_regions,
    std::vector<SampleKernelDescriptor> sample_kernels) {
    LowerResult result;
    auto contract = SampleRegionParameterContract::from_regions(sample_regions)
                        .freeze(std::span<const state::ParamInfo>{});
    if (!contract.valid()) {
        result.reason = LowerRejectReason::ParameterContractMismatch;
        result.message = contract.error();
        return result;
    }
    result.processor = std::unique_ptr<BakedGraphProcessor>(new BakedGraphProcessor(
        std::move(nodes), std::move(connections), input_channels, output_channels, std::move(name),
        std::move(bundle_id), std::move(custom_nodes), std::move(contract)));
    if (!sample_regions.empty()) {
        auto runtime = std::make_unique<detail::BakedSampleRegionRuntime>();
        runtime->definitions = std::move(sample_regions);
        runtime->kernels = std::move(sample_kernels);
        static_cast<BakedGraphProcessor*>(result.processor.get())->sample_region_runtime_ =
            std::move(runtime);
    }
    result.accepted = true;
    result.reason = LowerRejectReason::None;
    return result;
}

BakedGraphProcessor::BakedGraphProcessor(
    std::vector<GraphNode> nodes, std::vector<Connection> connections, int input_channels,
    int output_channels, std::string name, std::string bundle_id,
    std::unordered_map<NodeId, BakedCustomNodeBinding> custom_nodes,
    SampleRegionParameterContract parameter_contract)
    : nodes_(std::move(nodes)), conns_(std::move(connections)), name_(std::move(name)),
      bundle_id_(std::move(bundle_id)),
      sample_region_parameter_contract_(std::move(parameter_contract)),
      input_channels_(input_channels), output_channels_(output_channels) {
    // Latency is NOT computed here. A Custom node's intrinsic latency is a
    // function of the sample rate, which the constructor does not have, so the
    // graph total is derived in prepare() into prepared_latency_samples_.
    //
    // One single-writer mailbox per param-declaring node, allocated here (not in
    // prepare()) so a control-side claim/inject is valid before the first
    // prepare() and survives every re-prepare — the mailbox is fixed-capacity, so
    // this is the only allocation the injection path ever does.
    for (auto& [node_id, binding] : custom_nodes) {
        auto runtime = std::make_unique<detail::BakedCustomNodeRuntime>();
        runtime->process = std::move(binding.process);
        runtime->lifecycle = std::move(binding.lifecycle);
        runtime->params = std::move(binding.params);
        runtime->latency_samples = std::move(binding.latency_samples);
        if (!runtime->params.params.empty() && runtime->params.process) {
            runtime->mailbox = std::make_shared<detail::BakedParamMailbox>();
        }
        custom_nodes_.emplace(node_id, std::move(runtime));
    }
}

BakedGraphProcessor::~BakedGraphProcessor() = default;

fmt::PluginDescriptor BakedGraphProcessor::descriptor() const {
    fmt::PluginDescriptor desc;
    desc.name = name_;
    desc.bundle_id = bundle_id_;
    desc.category = fmt::PluginCategory::Effect;
    desc.input_buses = {{"Main In", input_channels_, false}};
    desc.output_buses = {{"Main Out", output_channels_, false}};
    return desc;
}

void BakedGraphProcessor::define_parameters(pulp::state::StateStore& store) {
    if (!sample_region_parameter_contract_.valid() || sample_region_parameter_binding_ != nullptr ||
        (sample_region_parameter_contract_.parameters().empty() && !sample_region_runtime_))
        return;
    // Validate an already-populated adapter store before attempting any
    // registration. StateStore has no rollback/remove operation, so adding
    // first would leave a conflicting publication behind when the exact
    // manifest check fails.
    if (store.param_count() != 0) {
        sample_region_parameter_binding_ = sample_region_parameter_contract_.bind(store);
        return;
    }
    for (const auto& parameter : sample_region_parameter_contract_.parameters())
        store.add_parameter(parameter);
    sample_region_parameter_binding_ = sample_region_parameter_contract_.bind(store);
}

void BakedGraphProcessor::prepare(const fmt::PrepareContext& context) {
    // Region preparation is a candidate transaction. Keep the accepted bank and
    // executor alive until every proof, binding and allocation has succeeded.
    if (!sample_region_runtime_) {
        prepared_ = false;
        prepared_latency_samples_.store(0, std::memory_order_relaxed);
        snapshot_.clear();
        pool_.clear();
    }
    std::unique_ptr<detail::BakedSampleRegionRuntime> candidate_runtime;
    if (sample_region_runtime_) {
        candidate_runtime = std::make_unique<detail::BakedSampleRegionRuntime>();
        candidate_runtime->definitions = sample_region_runtime_->definitions;
        candidate_runtime->kernels = sample_region_runtime_->kernels;
        if (!candidate_runtime->prepare(nodes_, conns_, sample_region_parameter_binding_.get(),
                                        context.sample_rate, context.max_buffer_size))
            return;
    }
    std::vector<std::unique_ptr<std::atomic<float>>> candidate_gains;
    std::unordered_map<NodeId, std::size_t> candidate_gain_index;
    fmt::GraphRuntimeSnapshot candidate_snapshot;
    fmt::GraphRuntimeBufferPool candidate_pool;
    std::vector<PluginBindingContext> candidate_plugin_ctx;
    std::vector<CustomBindingContext> candidate_custom_ctx;
    std::vector<float> candidate_alias_scratch;
    std::vector<float*> candidate_alias_ptrs;

    // One heap-stable atomic per Gain node, seeded from the baked value. The
    // routed Gain binding reads this atomic by address, so the storage must
    // outlive the snapshot — hence unique_ptr-indirected, never a value vector.
    for (const auto& node : nodes_) {
        if (node.type != NodeType::Gain) continue;
        candidate_gain_index[node.id] = candidate_gains.size();
        candidate_gains.push_back(std::make_unique<std::atomic<float>>(node.gain));
    }

    const int max_block = context.max_buffer_size;
    if (max_block <= 0) return;

    // Build the canonical executor's serialized routing snapshot for the frozen
    // plan, resolving each Gain node to its owned atomic and each lowerable Custom
    // node to its captured process callback. No Plugin nodes exist in the lowerable
    // subset, so plugin_for always yields nullptr.
    const ExecutorSnapshotBinders binders{
        .gain_for = [&candidate_gain_index, &candidate_gains](NodeId id) -> std::atomic<float>* {
            auto it = candidate_gain_index.find(id);
            return it == candidate_gain_index.end() ? nullptr : candidate_gains[it->second].get();
        },
        .plugin_for = [](NodeId) -> PluginSlot* { return nullptr; },
        .custom_for = [this, &candidate_runtime](NodeId id) -> const CustomNodeProcessFn* {
            if (candidate_runtime) {
                const auto region = candidate_runtime->processes.find(id);
                if (region != candidate_runtime->processes.end())
                    return &region->second;
            }
            auto it = custom_nodes_.find(id);
            return it == custom_nodes_.end() ? nullptr : &it->second->process;
        },
        .custom_latency_for =
            [this, sample_rate = context.sample_rate](NodeId id) {
                auto it = custom_nodes_.find(id);
                return it == custom_nodes_.end() || !it->second->latency_samples
                           ? 0
                           : std::max(0, it->second->latency_samples(sample_rate));
            },
    };
    const auto& executable_nodes = candidate_runtime ? candidate_runtime->nodes : nodes_;
    const auto& executable_connections =
        candidate_runtime ? candidate_runtime->connections : conns_;
    if (!build_executor_snapshot(executable_nodes, executable_connections, binders,
                                 candidate_plugin_ctx, plugin_scratch_, candidate_snapshot,
                                 /*parallel_safe=*/false, &candidate_custom_ctx)) {
        return;
    }

    // Size the scratch pool from the snapshot exactly as
    // build_signal_graph_executor_routing() does (slot count × max block, plus
    // per-connection PDC rings), so process_routed() is allocation-free.
    if (!candidate_pool.reset(candidate_snapshot.buffer_slot_count(),
                              static_cast<std::uint32_t>(max_block),
                              candidate_snapshot.buffer_assignment().connection_delay_samples)) {
        return;
    }
    // Size the in-place-host input scratch (one contiguous block, per-channel
    // pointers into it) so process() can rescue an aliased input with only a
    // copy_n on the audio thread. Sized for the descriptor's input bus — the
    // AudioInput gather never reads channels beyond input_channels_.
    candidate_alias_scratch.assign(
        static_cast<std::size_t>(input_channels_) * static_cast<std::size_t>(max_block), 0.0f);
    candidate_alias_ptrs.resize(static_cast<std::size_t>(input_channels_));
    for (int c = 0; c < input_channels_; ++c) {
        candidate_alias_ptrs[static_cast<std::size_t>(c)] =
            candidate_alias_scratch.data() +
            static_cast<std::size_t>(c) * static_cast<std::size_t>(max_block);
    }

    // Delay ordinary custom lifecycle mutation until region proof and executor
    // sizing have succeeded. Injection owns callback state, so refresh the
    // candidate's copied bindings after rebuilding it and before publication.
    for (auto& [id, runtime] : custom_nodes_) {
        auto& lifecycle = runtime->lifecycle;
        if (lifecycle.prepare)
            lifecycle.prepare(context.sample_rate, max_block);
        if (lifecycle.reset)
            lifecycle.reset();
        if (lifecycle.restore_state && !lifecycle.restore_state()) {
            // Legacy opaque callbacks cannot roll back their external state.
            prepared_ = false;
            return;
        }
    }
    prepare_param_injection();
    std::size_t custom_index = 0;
    for (const auto& node : candidate_snapshot.plan().nodes) {
        const auto authored = std::find_if(executable_nodes.begin(), executable_nodes.end(),
                                           [&](const auto& value) { return value.id == node.id; });
        if (authored == executable_nodes.end() || authored->type != NodeType::Custom)
            continue;
        if (const auto runtime = custom_nodes_.find(node.id); runtime != custom_nodes_.end())
            candidate_custom_ctx[custom_index].process = runtime->second->process;
        ++custom_index;
    }

    gains_ = std::move(candidate_gains);
    gain_index_ = std::move(candidate_gain_index);
    snapshot_ = std::move(candidate_snapshot);
    pool_ = std::move(candidate_pool);
    plugin_ctx_ = std::move(candidate_plugin_ctx);
    custom_ctx_ = std::move(candidate_custom_ctx);
    input_alias_scratch_ = std::move(candidate_alias_scratch);
    input_alias_ptrs_ = std::move(candidate_alias_ptrs);
    if (candidate_runtime)
        sample_region_runtime_ = std::move(candidate_runtime);
    prepared_max_block_ = max_block;
    const auto routed_latency = snapshot_.buffer_assignment().total_latency_samples;
    prepared_latency_samples_.store(
        static_cast<int>(std::min<std::uint32_t>(
            routed_latency, static_cast<std::uint32_t>(std::numeric_limits<int>::max()))),
        std::memory_order_relaxed);
    prepared_ = true;
}

void BakedGraphProcessor::process(
    pulp::audio::BufferView<float>& audio_output,
    const pulp::audio::BufferView<const float>& audio_input,
    pulp::midi::MidiBuffer& /*midi_in*/,
    pulp::midi::MidiBuffer& /*midi_out*/,
    const fmt::ProcessContext& context) {
    const auto frames = static_cast<std::uint32_t>(audio_output.num_samples());
    if (frames == 0) return;

    // The pool was sized for prepared_max_block_ frames; process_routed() reports
    // BufferPoolTooSmall for a larger block WITHOUT zeroing the output, so guard
    // here and emit silence rather than leave the caller's stale buffer intact.
    if (!prepared_ || static_cast<int>(frames) > prepared_max_block_) {
        audio_output.clear();
        return;
    }

    SampleRegionExecutionDomain::Admission admission;
    if (sample_region_runtime_) {
        const auto& bank = sample_region_runtime_->bank;
        if (!bank) {
            audio_output.clear();
            return;
        }
        admission = bank->domain().try_admit(bank->binding_generation());
        if (!admission) {
            audio_output.clear();
            return;
        }
        if (context.reset_requested)
            bank->reset();
    }

    // In-place hosts (Logic AUv2, some AUv3) hand process() input and output
    // views over the SAME memory. process_routed zeroes the main output bus
    // BEFORE its AudioInput gather reads the input bus, which would destroy an
    // aliased input and emit silence — so detect any input channel overlapping
    // any output channel and, when found, read the input from the scratch copy
    // sized in prepare(). Audio thread does only the pointer compares and the
    // copy_n: no allocation. Handles mono, multi-channel, and differing in/out
    // channel counts (the gather never reads past input_channels_, and channels
    // the view doesn't carry are zero-filled by the gather exactly as before).
    const std::size_t in_channels = audio_input.num_channels();
    const std::size_t out_channels = audio_output.num_channels();
    bool aliased = false;
    for (std::size_t i = 0; i < in_channels && !aliased; ++i) {
        const auto in_begin =
            reinterpret_cast<std::uintptr_t>(audio_input.channel_ptr(i));
        const auto in_end = in_begin + frames * sizeof(float);
        for (std::size_t o = 0; o < out_channels; ++o) {
            const auto out_begin =
                reinterpret_cast<std::uintptr_t>(audio_output.channel_ptr(o));
            const auto out_end = out_begin + frames * sizeof(float);
            if (in_begin < out_end && out_begin < in_end) {
                aliased = true;
                break;
            }
        }
    }
    pulp::audio::BufferView<const float> input_view = audio_input;
    if (aliased) {
        const std::size_t copy_channels =
            std::min(in_channels, input_alias_ptrs_.size());
        for (std::size_t c = 0; c < copy_channels; ++c) {
            std::copy_n(audio_input.channel_ptr(c), frames, input_alias_ptrs_[c]);
        }
        input_view = pulp::audio::BufferView<const float>(
            input_alias_ptrs_.data(), copy_channels, frames);
    }

    // Bridge the host's main in/out buffers into a ProcessBlock and run the
    // frozen plan through the canonical executor. The bus set + block are
    // stack-built (no allocation); process_routed gathers AudioInput from the
    // main input bus and writes AudioOutput to the main output bus.
    fmt::BusBufferSet buses;
    buses.add_input("main", input_view, fmt::BusRole::Main);
    buses.add_output("main", audio_output, fmt::BusRole::Main);

    fmt::ProcessBlock block;
    block.sample_rate = context.sample_rate;
    block.frame_count = frames;
    block.buses = &buses;
    if (!block.validate() || !executor_.process_routed(block, snapshot_, pool_).ok()) {
        audio_output.clear();
    }
}

} // namespace pulp::host
