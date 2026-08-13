#pragma once

// Custom-node extension vocabulary for the Pulp host signal graph. This focused
// header lets registrars describe custom nodes without importing graph runtime
// state or execution machinery.

#include <pulp/audio/buffer.hpp>
#include <pulp/state/parameter.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulp::format {
struct ProcessContext;
}  // namespace pulp::format

namespace pulp::host {

using CustomNodeProcessFn = std::function<void(audio::BufferView<float>& output,
                                              const audio::BufferView<const float>& input,
                                              int num_samples)>;

// ── Bake-layer parameter injection ──────────────────────────────────────
//
// A separate, RT-safe path (owned by BakedGraphProcessor) that lets a control
// thread set a BAKED custom node's parameter and have it applied sample-
// accurately in process(), WITHOUT re-baking and without touching the live
// graph's parameter-ingress path. It reuses pulp::state::ParameterEvent /
// ParameterEventQueue verbatim and mirrors the live mailbox's precedence and
// per-node exclusive-claim discipline; see baked_graph_processor.hpp.
//
// A lowerable custom node opts in by declaring `baked_params` and providing a
// `process_instance_baked_param` callback. On a BAKED graph the executor calls
// that callback instead of process_instance, handing it a BakedParamView it
// queries for the ramped, sample-accurate value of each declared param. This
// channel is consulted ONLY on a baked graph; on the live graph the node runs
// via its plain process_instance/process as usual (a node that provides only
// the baked callback runs neither live — the routed executor falls through to
// input passthrough — so it is meant to be baked before use).

// Read-only per-sample parameter accessor handed to a param-aware baked custom
// node. `value_at(id, k)` returns the ramped, sample-accurate value of a
// declared param at block-relative sample offset `k`; offsets must be queried
// in non-decreasing order within a block (the backing cursor advances
// monotonically). `value(id)` reads the value at the cursor's current position.
class BakedParamView {
public:
    virtual ~BakedParamView() = default;
    virtual float value_at(state::ParamID id, int32_t sample_offset) const = 0;
    virtual float value(state::ParamID id) const = 0;
};

// Bound param-aware process callback (instance captured). Same audio signature
// as CustomNodeProcessFn plus the block's BakedParamView. The baked executor
// runs this for a param-declaring custom node.
using CustomNodeParamProcessFn =
    std::function<void(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       int num_samples,
                       const BakedParamView& params)>;

// One declared parameter of a lowerable custom node, for the bake-layer
// injection path. `id` is node-local (the framework namespaces per node, so two
// nodes of the same type never collide). Values are clamped to [min, max];
// `default_value` is the held value until the first injection arrives.
struct CustomNodeBakedParam {
    state::ParamID id = 0;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
};

// Transport-aware custom-node callback (additive). Identical to
// CustomNodeProcessFn plus the host transport for the block. A custom type that
// registers one of the transport-aware callbacks below is treated as
// transport-sensitive: its routed binding forwards the live transport and it is
// excluded from the anticipation interior so it always runs live (see
// GraphNode::transport_sensitive).
using CustomNodeTransportProcessFn =
    std::function<void(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       int num_samples,
                       const format::ProcessContext& transport)>;

struct CustomNodeType {
    std::string type_id;
    int version = 1;
    int num_input_ports = 0;
    int num_output_ports = 0;
    std::string default_name;
    CustomNodeProcessFn process;  // stateless (used when `create` is empty)

    // Optional stateful lifecycle.
    // When `create` is set, the graph owns ONE opaque instance per node (RAII
    // via `destroy`). `process_instance` runs instead of `process`, and
    // prepare/release/reset/save_state/load_state operate on that instance. All
    // empty == today's stateless process-only node, byte-for-byte unchanged
    // (no instance is created and no state is serialized).
    //
    // Threading mirrors PluginSlot: create/prepare/release/save_state/load_state
    // are called on the UI/main thread (never from process()); process_instance
    // runs on the audio thread and must be real-time-safe. As with plugin
    // state, call save_state/load_state from non-audio control paths (graph not
    // live, or after invalidate + re-prepare).
    //
    // Parallel routing concurrency: the framework owns one DISTINCT instance per
    // node, so two nodes of the same type never share instance state. But under
    // levelized parallel routing, process_instance for sibling nodes in a level
    // may run on different worker threads at once — so a callback that touches
    // state SHARED across nodes (e.g. a captured global, not the per-instance
    // pointer) must itself be concurrent-safe, or that graph must not enable
    // parallel routing. The per-instance `void*` is always single-threaded.
    std::function<void*()> create;
    std::function<void(void* /*instance*/)> destroy;
    std::function<void(void* /*instance*/, double /*sample_rate*/, int /*max_block*/)> prepare;
    std::function<void(void* /*instance*/)> release;
    std::function<void(void* /*instance*/)> reset;
    std::function<void(void* /*instance*/, audio::BufferView<float>& /*output*/,
                       const audio::BufferView<const float>& /*input*/,
                       int /*num_samples*/)>
        process_instance;
    std::function<std::vector<uint8_t>(void* /*instance*/)> save_state;
    std::function<bool(void* /*instance*/, const std::vector<uint8_t>& /*bytes*/)> load_state;

    // Optional transport-aware callbacks (additive opt-in). When either is set,
    // the node is transport-sensitive: the graph forwards the host transport to
    // it and excludes it from the anticipation interior so it always runs live.
    // `process_transport` is the stateless variant (used when `create` is empty);
    // `process_instance_transport` is the stateful variant (leading instance
    // pointer, used when an instance exists). Both default-empty == today's
    // transport-unaware node, byte-for-byte unchanged. A type that sets a
    // transport-aware callback alongside its plain `process`/`process_instance`
    // gets the transport variant on the routed path; the plain one remains the
    // fallback when no transport is available for the block.
    CustomNodeTransportProcessFn process_transport;  // stateless
    std::function<void(void* /*instance*/, audio::BufferView<float>& /*output*/,
                       const audio::BufferView<const float>& /*input*/,
                       int /*num_samples*/, const format::ProcessContext& /*transport*/)>
        process_instance_transport;

    // Bake opt-in: whether this type may be LOWERED into a baked artifact.
    // Default false → bake refuses (a custom instance holds opaque state a frozen
    // topology cannot otherwise capture). Setting true is a registrar assertion
    // that the type is deterministic given (state, input), holds no ambient mutable
    // global state (only the per-instance void*), round-trips completely through
    // save_state/load_state, is real-time-safe, and is NOT transport-sensitive.
    // This is a trusted-binary / developer boundary only — NOT a shipped-artifact
    // security boundary (the framework cannot verify these properties), so the
    // on-disk load path must additionally require a signature before honoring it.
    bool lowerable = false;

    // Bake-layer parameter injection (additive opt-in). When `baked_params` is
    // non-empty AND `process_instance_baked_param` is set, a BAKED instance of
    // this node can receive sample-accurate ParameterEvents from the control
    // thread via BakedGraphProcessor::inject(); the baked executor runs
    // `process_instance_baked_param` (leading instance pointer) instead of
    // process_instance, handing it a BakedParamView over the injected values.
    // Both empty == today's node, unchanged: no injection channel, runs via
    // process_instance. This path is independent of the live-graph parameter
    // ingress and does not require re-baking to turn a knob.
    //
    // Live-graph behavior: `process_instance_baked_param` is consulted ONLY on a
    // baked graph. A node that provides ONLY this callback (no plain
    // process/process_instance) does NOT run its baked-param DSP on the live
    // graph — with no live callback the routed executor falls through to input
    // passthrough (transparent; it contributes no signal of its own, but is not
    // hard silence when fed a signal). Such a node is meant to be baked before
    // use; provide a plain process callback too if it must also run live.
    std::vector<CustomNodeBakedParam> baked_params;
    std::function<void(void* /*instance*/, audio::BufferView<float>& /*output*/,
                       const audio::BufferView<const float>& /*input*/,
                       int /*num_samples*/, const BakedParamView& /*params*/)>
        process_instance_baked_param;

    // Prepare-stable intrinsic latency metadata. Evaluated once, off the audio
    // thread, at the graph's sample rate during compilation; the non-negative
    // result feeds both the legacy and routed PDC plans. The callback must depend
    // only on registration-time realization choices and `sample_rate` — a
    // control that can change it requires a new type / re-prepare. Empty
    // preserves the historical zero-latency custom-node contract.
    //
    // A callback rather than a fixed count because intrinsic latency is
    // routinely rate-dependent: a lookahead declared in milliseconds and an
    // oversampler's half-band group delay both resolve to different sample
    // counts per rate, so a fixed count could only ever be right at one of them.
    // A rate-independent latency stays expressible as `[](double) { return n; }`.
    //
    // Kept last to preserve source compatibility for positional aggregate
    // initializers written before the latency contract was added. The graph
    // captures it at prepare; changing it requires re-registration/re-prepare
    // and an identity version bump for persisted graphs.
    //
    // The EVALUATED result is range-checked against `kMaxLatencySamples` at
    // compile time. It cannot be checked at registration, because it is not
    // known until the sample rate is.
    static constexpr int kMaxLatencySamples = 65535;
    std::function<int(double /*sample_rate*/)> latency_samples;

    bool is_valid_registration() const noexcept {
        const bool has_plain_callback =
            static_cast<bool>(process) ||
            (static_cast<bool>(create) && static_cast<bool>(process_instance));
        const bool has_transport_callback =
            static_cast<bool>(process_transport) ||
            (static_cast<bool>(create) &&
             static_cast<bool>(process_instance_transport));
        // A latent transport-aware execution path must have a resolvable plain
        // fallback: PDC is prepare-stable and cannot change when a block gains
        // or loses a transport context. Declaring ANY latency callback is the
        // registration-time stand-in for a non-zero fixed latency, since the
        // value itself is not resolvable this early.
        if (type_id.empty() || version <= 0 || num_input_ports < 0 ||
            num_output_ports < 0 ||
            (latency_samples && has_transport_callback && !has_plain_callback)) {
            return false;
        }

        // Bake-layer obligations live here too, so that every registrar — the
        // direct one, the transactional edit, and the node-adding leaf — applies
        // ONE rule. A descriptor that passes shape but declares an incoherent
        // bake surface would otherwise stay `lowerable`, be accepted by baking,
        // and silently degrade to passthrough.
        const bool declares_baked_params = !baked_params.empty();
        const bool has_baked_param_process =
            static_cast<bool>(process_instance_baked_param);
        if (declares_baked_params != has_baked_param_process) return false;

        if (declares_baked_params) {
            // The callback takes an instance pointer, so a parameter-aware baked
            // type without a complete instance lifecycle can never produce the
            // runnable binding that bake() promises.
            if (!create || !destroy) return false;
            for (std::size_t i = 0; i < baked_params.size(); ++i) {
                const auto& param = baked_params[i];
                if (param.id == 0
                    || !std::isfinite(param.min_value)
                    || !std::isfinite(param.max_value)
                    || !std::isfinite(param.default_value)
                    || param.min_value > param.max_value
                    || param.default_value < param.min_value
                    || param.default_value > param.max_value) {
                    return false;
                }
                for (std::size_t j = 0; j < i; ++j) {
                    if (baked_params[j].id == param.id) return false;
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
};

}  // namespace pulp::host
