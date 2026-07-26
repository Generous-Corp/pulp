#include <pulp/host/baked_graph_processor.hpp>
#include "baked_graph_processor_detail.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace pulp::host {

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

    if (!signal_graph_topology_executor_eligible(nodes, connections)) {
        proof.reason = LowerRejectReason::NotExecutorEligible;
        proof.message =
            "graph is outside the routed executor's bit-exact subset; refusing to bake";
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

    // Topology lowerability — the shared gate (see lowerability_of). bake()'s only
    // extra precondition is is_prepared() above; the node-kind / lane / executor-
    // eligibility proof is identical to what the on-disk load path will re-run.
    if (const auto proof = lowerability_of(
            graph.nodes(), graph.connections(),
            [&graph](std::string_view type_id, int version) {
                return graph.custom_node_type(type_id, version);
            });
        !proof.accepted) {
        result.reason = proof.reason;
        result.offending_node = proof.offending_node;
        result.message = proof.message;
        return result;
    }

    // Accepted: capture the plan into owned storage. Copy each node's identity +
    // arity (the snapshot builder resolves connections by NodeId and reads ports
    // off these specs), the gain for each Gain node via the public node_gain()
    // accessor, and the connection list verbatim. Derive the bus arity from the
    // AudioInput/AudioOutput nodes.
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
            if (const CustomNodeType* type = graph.custom_node_type(
                    src.custom_type_id, src.custom_type_version);
                type != nullptr && type->latency_samples) {
                custom_nodes[src.id].latency_samples = type->latency_samples;
            }
            if (const CustomNodeProcessFn* fn = graph.live_custom_processor(src.id)) {
                custom_nodes[src.id].process = *fn;
            }
            if (src.custom_instance) {
                const CustomNodeType* type =
                    graph.custom_node_type(src.custom_type_id, src.custom_type_version);
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
                if (const CustomNodeType* type = graph.custom_node_type(
                        src.custom_type_id, src.custom_type_version)) {
                    custom_nodes[src.id].params =
                        BakedCustomParamBinding{*pfn, type->baked_params};
                }
            }
        }
        nodes.push_back(std::move(n));
    }
    std::vector<Connection> conns(graph.connections().begin(), graph.connections().end());

    result.processor = std::make_unique<BakedGraphProcessor>(
        std::move(nodes), std::move(conns),
        input_channels > 0 ? input_channels : 2,
        output_channels > 0 ? output_channels : 2,
        "Baked Graph", "com.pulp.baked-graph", std::move(custom_nodes));
    result.accepted = true;
    result.reason = LowerRejectReason::None;
    return result;
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
    if (const auto proof = lowerability_of(
            graph.nodes(), graph.connections(),
            [&graph](std::string_view type_id, int version) {
                return graph.custom_node_type(type_id, version);
            });
        !proof.accepted) {
        result.reason = proof.reason;
        result.offending_node = proof.offending_node;
        result.message = proof.message;
        return result;
    }
    BakedPlan plan;
    plan.format_version = kBakedPlanFormatVersion;
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

BakedGraphProcessor::BakedGraphProcessor(
    std::vector<GraphNode> nodes,
    std::vector<Connection> connections,
    int input_channels,
    int output_channels,
    std::string name,
    std::string bundle_id,
    std::unordered_map<NodeId, BakedCustomNodeBinding> custom_nodes)
    : nodes_(std::move(nodes)),
      conns_(std::move(connections)),
      name_(std::move(name)),
      bundle_id_(std::move(bundle_id)),
      input_channels_(input_channels),
      output_channels_(output_channels) {
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

void BakedGraphProcessor::define_parameters(pulp::state::StateStore& /*store*/) {
    // The lowerable subset (AudioInput/AudioOutput/Gain) exposes no host
    // parameters in this slice: a Gain's value is frozen into the plan at bake().
}

void BakedGraphProcessor::prepare(const fmt::PrepareContext& context) {
    prepared_ = false;
    prepared_latency_samples_.store(0, std::memory_order_relaxed);
    snapshot_.clear();
    pool_.clear();
    gains_.clear();
    gain_index_.clear();

    // One heap-stable atomic per Gain node, seeded from the baked value. The
    // routed Gain binding reads this atomic by address, so the storage must
    // outlive the snapshot — hence unique_ptr-indirected, never a value vector.
    for (const auto& node : nodes_) {
        if (node.type != NodeType::Gain) continue;
        gain_index_[node.id] = gains_.size();
        gains_.push_back(std::make_unique<std::atomic<float>>(node.gain));
    }

    const int max_block = context.max_buffer_size;
    if (max_block <= 0) return;

    // Re-init each stateful Custom instance BEFORE the snapshot goes live,
    // mirroring SignalGraph::prepare()'s own instance step: re-run the type's
    // prepare at the host's REAL rate/block (bake / load_baked may have prepared
    // at a different or nominal rate), then reset so no stale DSP state — e.g. a
    // delay line still holding the source graph's audio — leaks into the baked
    // stream. A signed artifact may additionally carry authenticated initial
    // state; restore it LAST so prepare/reset cannot erase it. In-memory bake
    // has no restore hook and retains the fresh-stream contract. Control-thread
    // only, never on the audio path.
    // Runs BEFORE prepare_param_injection() so the injection state's reset of
    // held params to declared defaults lands on a freshly re-inited instance —
    // one coherent re-init boundary for both DSP state and injected params.
    for (auto& [id, runtime] : custom_nodes_) {
        auto& lc = runtime->lifecycle;
        if (lc.prepare) lc.prepare(context.sample_rate, max_block);
        if (lc.reset) lc.reset();
        if (lc.restore_state && !lc.restore_state()) return;
    }

    // Build per-node injection state for every param-declaring custom node and
    // install a draining wrapper into each Custom runtime so the routed executor
    // invokes the injection path. This MUST run before build_executor_snapshot
    // (which resolves each Custom node through custom_nodes_). All the sizing
    // — StateStore registration, the fixed-capacity scratch queue, held-value
    // storage — happens here, off the audio thread, so process() never allocates.
    prepare_param_injection();

    // Build the canonical executor's serialized routing snapshot for the frozen
    // plan, resolving each Gain node to its owned atomic and each lowerable Custom
    // node to its captured process callback. No Plugin nodes exist in the lowerable
    // subset, so plugin_for always yields nullptr.
    const ExecutorSnapshotBinders binders{
        .gain_for =
            [this](NodeId id) -> std::atomic<float>* {
                auto it = gain_index_.find(id);
                return it == gain_index_.end() ? nullptr : gains_[it->second].get();
            },
        .plugin_for = [](NodeId) -> PluginSlot* { return nullptr; },
        .custom_for =
            [this](NodeId id) -> const CustomNodeProcessFn* {
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
    if (!build_executor_snapshot(nodes_, conns_, binders, plugin_ctx_,
                                 plugin_scratch_, snapshot_,
                                 /*parallel_safe=*/false, &custom_ctx_)) {
        return;
    }

    // Size the scratch pool from the snapshot exactly as
    // build_signal_graph_executor_routing() does (slot count × max block, plus
    // per-connection PDC rings), so process_routed() is allocation-free.
    if (!pool_.reset(snapshot_.buffer_slot_count(),
                     static_cast<std::uint32_t>(max_block),
                     snapshot_.buffer_assignment().connection_delay_samples)) {
        return;
    }
    // Size the in-place-host input scratch (one contiguous block, per-channel
    // pointers into it) so process() can rescue an aliased input with only a
    // copy_n on the audio thread. Sized for the descriptor's input bus — the
    // AudioInput gather never reads channels beyond input_channels_.
    input_alias_scratch_.assign(static_cast<std::size_t>(input_channels_) *
                                    static_cast<std::size_t>(max_block),
                                0.0f);
    input_alias_ptrs_.resize(static_cast<std::size_t>(input_channels_));
    for (int c = 0; c < input_channels_; ++c) {
        input_alias_ptrs_[static_cast<std::size_t>(c)] =
            input_alias_scratch_.data() +
            static_cast<std::size_t>(c) * static_cast<std::size_t>(max_block);
    }

    prepared_max_block_ = max_block;
    const auto routed_latency = snapshot_.buffer_assignment().routed_output_latency_samples;
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
