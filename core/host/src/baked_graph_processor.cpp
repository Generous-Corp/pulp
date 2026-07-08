#include <pulp/host/baked_graph_processor.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace pulp::host {
namespace {
namespace fmt = pulp::format;
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

LowerResult bake(const SignalGraph& graph) {
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
    if (const auto proof = lowerability_of(graph.nodes(), graph.connections());
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
    int input_channels = 0;
    int output_channels = 0;
    for (const auto& src : graph.nodes()) {
        GraphNode n;
        n.id = src.id;
        n.type = src.type;
        n.name = src.name;
        n.num_input_ports = src.num_input_ports;
        n.num_output_ports = src.num_output_ports;
        if (src.type == NodeType::Gain) {
            n.gain = graph.node_gain(src.id);
        } else if (src.type == NodeType::AudioInput) {
            input_channels = std::max(input_channels, src.num_output_ports);
        } else if (src.type == NodeType::AudioOutput) {
            output_channels = std::max(output_channels, src.num_input_ports);
        }
        nodes.push_back(std::move(n));
    }
    std::vector<Connection> conns(graph.connections().begin(), graph.connections().end());

    result.processor = std::make_unique<BakedGraphProcessor>(
        std::move(nodes), std::move(conns),
        input_channels > 0 ? input_channels : 2,
        output_channels > 0 ? output_channels : 2,
        "Baked Graph", "com.pulp.baked-graph");
    result.accepted = true;
    result.reason = LowerRejectReason::None;
    return result;
}

BakedGraphProcessor::BakedGraphProcessor(std::vector<GraphNode> nodes,
                                         std::vector<Connection> connections,
                                         int input_channels,
                                         int output_channels,
                                         std::string name,
                                         std::string bundle_id)
    : nodes_(std::move(nodes)),
      conns_(std::move(connections)),
      name_(std::move(name)),
      bundle_id_(std::move(bundle_id)),
      input_channels_(input_channels),
      output_channels_(output_channels) {}

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

    // Build the canonical executor's serialized routing snapshot for the frozen
    // plan, resolving each Gain node to its owned atomic. No Plugin nodes exist
    // in the lowerable subset, so plugin_for always yields nullptr.
    if (!build_executor_snapshot(
            nodes_, conns_,
            [this](NodeId id) -> std::atomic<float>* {
                auto it = gain_index_.find(id);
                return it == gain_index_.end() ? nullptr : gains_[it->second].get();
            },
            [](NodeId) -> PluginSlot* { return nullptr; },
            plugin_ctx_, plugin_scratch_, snapshot_, /*parallel_safe=*/false)) {
        return;
    }

    // Size the scratch pool from the snapshot exactly as
    // build_signal_graph_executor_routing() does (slot count × max block, plus
    // per-connection PDC rings), so process_routed() is allocation-free.
    const int max_block = context.max_buffer_size;
    if (max_block <= 0) return;
    if (!pool_.reset(snapshot_.buffer_slot_count(),
                     static_cast<std::uint32_t>(max_block),
                     snapshot_.buffer_assignment().connection_delay_samples)) {
        return;
    }

    prepared_max_block_ = max_block;
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

    // Bridge the host's main in/out buffers into a ProcessBlock and run the
    // frozen plan through the canonical executor. The bus set + block are
    // stack-built (no allocation); process_routed gathers AudioInput from the
    // main input bus and writes AudioOutput to the main output bus.
    fmt::BusBufferSet buses;
    buses.add_input("main", audio_input, fmt::BusRole::Main);
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
