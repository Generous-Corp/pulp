// SignalGraph private compile seam: runtime edge bucketing, canonical ordering and telemetry.
#include "signal_graph_internal.hpp"
#include <pulp/host/signal_graph_runtime.hpp>
#include <algorithm>
#include <cstddef>
#include <tuple>
#include <vector>

namespace pulp::host {

void SignalGraph::assemble_runtime_(CompiledGraph& compiled, int max_block_size) {
    CompiledGraph* const cg = &compiled;
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
}

} // namespace pulp::host
