// signal_graph_live_swap.cpp — SignalGraph's LIVE-SWAP TRANSACTION ENGINE.
//
// Holds everything behind the no-silence topology edit: the scanned-plugin
// catalog and per-node swap policy, the staged-replacement pipeline
// (stage_plugin_replacement: load → prepare → contract/shape/latency gates →
// warm-up cost probe → state restore), the swap-edit transaction
// (begin_swap_edit / prepare_swap / abort_swap_edit) with its crossfade publish
// and rollback, the reinit-free predicate that decides whether a candidate
// topology can be published without silencing the live snapshot, and the
// admission gate that refuses a swap the current audio load cannot absorb.
//
// The members stay SignalGraph members (they name the private nested
// CompiledGraph / PreparedPluginMetadata / StagedReplacement types and the
// transaction state guarded by graph_mutation_mutex_); only their definitions
// live here, so signal_graph.cpp keeps the topology/compile/process spine. Same
// arrangement as signal_graph_reference_walk.cpp.
//
// Every function here runs on the CONTROL thread. The `_locked_` suffix marks a
// helper whose caller must already hold graph_mutation_mutex_; the public
// entry points take it.

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/crossfade_plugin_slot.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include "signal_graph_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::host {
namespace {

constexpr std::uint64_t kLiveSwapMinAdmissionCallbacks = 8;
constexpr std::uint64_t kLiveSwapWarmBlocks = 16;
static_assert(kLiveSwapWarmBlocks >= kLiveSwapMinAdmissionCallbacks);
// The warm-up estimate is measured on the control thread with a fixed test signal,
// so it can under-read the real audio-thread cost (scheduling, thermal, and
// signal-dependent paths the probe noise doesn't hit). Scale it up before it faces
// the admission headroom so a borderline replacement fail-closes to eager-prepare
// rather than risk an xrun. Combined with admission's already-conservative
// whole-graph-plus-full-new-node projection, this keeps different-instance swaps safe.
constexpr float kLiveSwapWarmSafetyMargin = 1.5f;
constexpr int kLiveSwapMinFadeMs = 5;
constexpr int kLiveSwapMaxFadeMs = 200;

NodeLiveSwapPolicy clamp_live_swap_policy(NodeLiveSwapPolicy policy) {
    // fade_ms == 0 is the explicit "instant switch, no crossfade" request and passes
    // through unclamped; any positive value is clamped into the supported fade range.
    if (policy.fade_ms != 0) {
        policy.fade_ms = std::clamp(policy.fade_ms,
                                    kLiveSwapMinFadeMs,
                                    kLiveSwapMaxFadeMs);
    }
    policy.headroom_threshold =
        std::clamp(policy.headroom_threshold, 0.0f, 1.0f);
    return policy;
}

std::string normalized_plugin_path(const std::string& path) {
    if (path.empty()) return {};
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    const fs::path weak = fs::weakly_canonical(p, ec);
    if (!ec) return weak.string();
    ec.clear();
    const fs::path abs = fs::absolute(p, ec);
    if (!ec) return abs.lexically_normal().string();
    return p.lexically_normal().string();
}

std::string plugin_identity_key(const PluginInfo& info) {
    std::string key = normalized_plugin_path(info.path);
    key += '\n';
    key += std::to_string(static_cast<int>(info.format));
    key += '\n';
    key += info.unique_id;
    key += '\n';
    key += info.is_effect ? 'E' : 'e';
    key += info.is_instrument ? 'I' : 'i';
    return key;
}

bool same_plugin_identity(const PluginInfo& a, const PluginInfo& b) {
    return plugin_identity_key(a) == plugin_identity_key(b);
}

struct ComparableParam {
    uint32_t id = 0;
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
    ParamFlags flags;
    state::ParamRate rate = state::ParamRate::ControlRate;
};

std::vector<ComparableParam>
comparable_params(std::vector<HostParamInfo> params) {
    std::vector<ComparableParam> out;
    out.reserve(params.size());
    for (const auto& p : params) {
        out.push_back({
            p.id,
            p.min_value,
            p.max_value,
            p.default_value,
            p.flags,
            p.rate,
        });
    }
    std::sort(out.begin(), out.end(),
              [](const ComparableParam& a, const ComparableParam& b) {
                  return a.id < b.id;
              });
    return out;
}

bool same_param_flags(const ParamFlags& a, const ParamFlags& b) {
    return a.automatable == b.automatable
        && a.read_only == b.read_only
        && a.hidden == b.hidden
        && a.stepped == b.stepped
        && a.is_bypass == b.is_bypass
        && a.rampable == b.rampable
        && a.modulatable == b.modulatable;
}

bool same_parameter_contract(std::vector<HostParamInfo> a,
                             std::vector<HostParamInfo> b) {
    auto ca = comparable_params(std::move(a));
    auto cb = comparable_params(std::move(b));
    if (ca.size() != cb.size()) return false;
    for (std::size_t i = 0; i < ca.size(); ++i) {
        if (ca[i].id != cb[i].id) return false;
        if (ca[i].min_value != cb[i].min_value) return false;
        if (ca[i].max_value != cb[i].max_value) return false;
        if (ca[i].default_value != cb[i].default_value) return false;
        if (ca[i].rate != cb[i].rate) return false;
        if (!same_param_flags(ca[i].flags, cb[i].flags)) return false;
    }
    return true;
}

const char* live_swap_reason_name(LiveSwapFallbackReason reason) {
    switch (reason) {
    case LiveSwapFallbackReason::None: return "None";
    case LiveSwapFallbackReason::NotOptedIn: return "NotOptedIn";
    case LiveSwapFallbackReason::LoadFailed: return "LoadFailed";
    case LiveSwapFallbackReason::PrepareFailed: return "PrepareFailed";
    case LiveSwapFallbackReason::StateRestoreFailed: return "StateRestoreFailed";
    case LiveSwapFallbackReason::StateTooLarge: return "StateTooLarge";
    case LiveSwapFallbackReason::ShapeMismatch: return "ShapeMismatch";
    case LiveSwapFallbackReason::LatencyChanged: return "LatencyChanged";
    case LiveSwapFallbackReason::EditorOpen: return "EditorOpen";
    case LiveSwapFallbackReason::ParamContractMismatch:
        return "ParamContractMismatch";
    case LiveSwapFallbackReason::FeedbackNotSwappable:
        return "FeedbackNotSwappable";
    case LiveSwapFallbackReason::OverBudget: return "OverBudget";
    case LiveSwapFallbackReason::NoLoadHistory: return "NoLoadHistory";
    case LiveSwapFallbackReason::PredicateExcluded: return "PredicateExcluded";
    case LiveSwapFallbackReason::UntrustedIdentity: return "UntrustedIdentity";
    }
    return "Unknown";
}

} // namespace

LiveSwapAdmission evaluate_live_swap_admission(
    const pulp::audio::AudioProcessLoadSnapshot& graph,
    const std::vector<pulp::audio::AudioProcessLoadSnapshot>& staged_nodes,
    float headroom_threshold, std::uint64_t min_callbacks) {
    // Deny on uncertainty: an unmeasured graph or node could xrun under a doubled
    // fade render, and eager-silence is the safe fallback.
    if (graph.callback_count < min_callbacks) return {false, "no history"};
    for (const auto& n : staged_nodes) {
        if (n.callback_count < min_callbacks) return {false, "no history"};
    }
    // Projected peak = current worst-case graph load + the fade's added second render
    // of each staged node (its own worst-case load), all as callback-budget fractions.
    float projected = std::max(graph.load, graph.last_load);
    for (const auto& n : staged_nodes) projected += std::max(n.load, n.last_load);
    if (projected > headroom_threshold) return {false, "over budget"};
    return {true, "ok"};
}

bool SignalGraph::set_node_live_swap_policy(NodeId id,
                                            NodeLiveSwapPolicy policy) {
    GraphMutationLock mutation_lock(*this);
    auto* n = node_mut_locked_(id);
    if (!n || n->type != NodeType::Plugin) return false;
    n->live_swap_policy = clamp_live_swap_policy(std::move(policy));
    ++authoring_generation_;
    return true;
}

bool SignalGraph::set_node_hosted_editor_open(NodeId id, bool open) {
    GraphMutationLock mutation_lock(*this);
    auto* n = node_mut_locked_(id);
    if (!n || n->type != NodeType::Plugin) return false;
    n->hosted_editor_open = open;
    ++authoring_generation_;
    return true;
}

SignalGraph::PluginCatalogToken
SignalGraph::register_scanned_plugin(const PluginInfo& info) {
    GraphMutationLock mutation_lock(*this);
    const auto key = plugin_identity_key(info);
    if (auto it = scanned_plugin_identity_to_token_.find(key);
        it != scanned_plugin_identity_to_token_.end()) {
        return PluginCatalogToken{it->second};
    }
    const std::uint64_t token = next_scanned_plugin_token_++;
    scanned_plugin_catalog_[token] = info;
    scanned_plugin_identity_to_token_[key] = token;
    return PluginCatalogToken{token};
}

void SignalGraph::clear_scanned_plugin_catalog() {
    GraphMutationLock mutation_lock(*this);
    scanned_plugin_catalog_.clear();
    scanned_plugin_identity_to_token_.clear();
}

LiveSwapDiagnostics SignalGraph::last_swap_diagnostics() const {
    GraphMutationLock mutation_lock(*this);
    return last_swap_diagnostics_;
}

void SignalGraph::set_live_swap_plugin_loader_for_test(
    PluginLoaderForTest loader) {
    GraphMutationLock mutation_lock(*this);
    live_swap_plugin_loader_for_test_ = std::move(loader);
}

SignalGraph::SwapResult
SignalGraph::stage_plugin_replacement(NodeId id, PluginCatalogToken token) {
    GraphMutationLock mutation_lock(*this);
    if (!in_swap_edit_) return SwapResult::NotInSwapEdit;

    const auto catalog_it = scanned_plugin_catalog_.find(token.value);
    if (!token || catalog_it == scanned_plugin_catalog_.end()) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::UntrustedIdentity,
                                      id,
                                      "replacement token is not in the scan catalog");
    }

    auto* n = node_mut_locked_(id);
    if (!n || n->type != NodeType::Plugin || !n->plugin || !live_slot_.live()) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::PredicateExcluded,
                                      id,
                                      "node is not a prepared plugin node");
    }

    const NodeLiveSwapPolicy policy = n->live_swap_policy;
    if (!policy.allow_live_instance_swap) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::NotOptedIn,
                                      id,
                                      "node has not opted into live instance swap");
    }
    if (node_has_feedback_edge_locked_(id)) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::FeedbackNotSwappable,
                                      id,
                                      "node participates in a feedback edge");
    }
    if (n->hosted_editor_open) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::EditorOpen,
                                      id,
                                      "hosted editor is open");
    }

    const auto old_meta_it = prepared_plugin_meta_.find(id);
    if (old_meta_it == prepared_plugin_meta_.end()) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::PredicateExcluded,
                                      id,
                                      "prepared plugin metadata is unavailable");
    }
    const auto live_shape_it = live_slot_.live()->shapes.find(id);
    if (live_shape_it == live_slot_.live()->shapes.end()) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::PredicateExcluded,
                                      id,
                                      "live snapshot shape is unavailable");
    }

    const PluginInfo replacement_info = catalog_it->second;
    auto loaded = load_live_swap_plugin_locked_(replacement_info);
    if (!loaded) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::LoadFailed,
                                      id,
                                      "replacement plugin did not load");
    }
    if (!loaded->prepare(live_slot_.live()->sample_rate, live_slot_.live()->max_block_size)) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::PrepareFailed,
                                      id,
                                      "replacement plugin did not prepare");
    }

    const bool same_identity = same_plugin_identity(n->plugin_info,
                                                   replacement_info);
    std::vector<HostParamInfo> new_params = loaded->parameters();
    if (node_has_parameter_or_automation_contract_locked_(id)
        && !same_identity
        && !same_parameter_contract(old_meta_it->second.parameters, new_params)) {
        return fail_swap_edit_locked_(
            LiveSwapFallbackReason::ParamContractMismatch,
            id,
            "replacement parameter contract differs from the live node");
    }

    const std::vector<std::uint8_t> old_state = n->plugin->save_state();
    if (old_state.size() > policy.max_state_bytes) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::StateTooLarge,
                                      id,
                                      "saved plugin state exceeds the policy limit");
    }

    const PluginInfo loaded_info = loaded->info();
    const auto& live_shape = live_shape_it->second;
    if (live_shape.type != NodeType::Plugin ||
        live_shape.num_input_ports != loaded_info.num_inputs ||
        live_shape.num_output_ports != loaded_info.num_outputs) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::ShapeMismatch,
                                      id,
                                      "replacement port shape differs from the live node");
    }

    const int new_latency = std::max(0, loaded->latency_samples());
    if (new_latency != old_meta_it->second.latency_samples) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::LatencyChanged,
                                      id,
                                      "replacement latency differs from the live node");
    }
    const bool wants_transport = loaded->wants_transport();

    // Estimate CPU cost on a THROWAWAY probe instance — never the instance that goes
    // live. The warm-up render dirties internal DSP memory (filter/delay/reverb state)
    // that save_state()/restore_state() do not round-trip, so warming `loaded` directly
    // would commit an instance carrying warm-up residue. A fresh probe is loaded +
    // prepared, measured, and discarded here on the control thread; `loaded` is only
    // ever state-restored, so it goes live clean. If the probe can't load/prepare, the
    // estimate stays empty (callback_count 0) and admission fail-closes to eager-prepare.
    pulp::audio::AudioProcessLoadSnapshot warmed_load;
    if (auto probe = load_live_swap_plugin_locked_(replacement_info)) {
        if (probe->prepare(live_slot_.live()->sample_rate, live_slot_.live()->max_block_size)) {
            warmed_load = warm_staged_slot_locked_(*probe, id);
        }
    }
    // Conservative margin over the control-thread noise proxy: the audio thread may run
    // hotter (contention/thermal/signal-dependence the test noise doesn't exercise), so
    // scale the estimate up before it faces the admission headroom.
    warmed_load.load = std::min(1.0f, warmed_load.load * kLiveSwapWarmSafetyMargin);
    warmed_load.last_load = std::min(1.0f, warmed_load.last_load * kLiveSwapWarmSafetyMargin);
    warmed_load.peak_load = std::min(1.0f, warmed_load.peak_load * kLiveSwapWarmSafetyMargin);

    if (!loaded->restore_state(old_state)) {
        return fail_swap_edit_locked_(LiveSwapFallbackReason::StateRestoreFailed,
                                      id,
                                      "replacement plugin rejected the live state");
    }

    StagedReplacement staged;
    staged.id = id;
    staged.info = loaded_info;
    staged.metadata = PreparedPluginMetadata{
        std::move(new_params),
        new_latency,
        wants_transport,
    };
    staged.warmed_load = warmed_load;
    staged.same_identity = same_identity;
    staged.slot = std::shared_ptr<PluginSlot>(std::move(loaded));
    staged_replacements_[id] = std::move(staged);
    set_live_swap_diagnostics_locked_(LiveSwapFallbackReason::None, 0, {});
    return SwapResult::Staged;
}

std::unique_ptr<PluginSlot>
SignalGraph::load_live_swap_plugin_locked_(const PluginInfo& info) const {
    assert_graph_mutation_locked_();
    if (live_swap_plugin_loader_for_test_) {
        return live_swap_plugin_loader_for_test_(info);
    }
    return PluginSlot::load(info);
}

pulp::audio::AudioProcessLoadSnapshot
SignalGraph::warm_staged_slot_locked_(PluginSlot& slot, NodeId id) const {
    assert_graph_mutation_locked_();
    if (!live_slot_.live()) {
        return pulp::audio::AudioProcessLoadSnapshot{};
    }
    const auto shape_it = live_slot_.live()->shapes.find(id);
    if (shape_it == live_slot_.live()->shapes.end()) {
        return pulp::audio::AudioProcessLoadSnapshot{};
    }

    const auto& shape = shape_it->second;
    const int block_size = live_slot_.live()->max_block_size;
    const float sample_rate = static_cast<float>(live_slot_.live()->sample_rate);
    if (block_size <= 0 || !(sample_rate > 0.0f)) {
        return pulp::audio::AudioProcessLoadSnapshot{};
    }

    pulp::audio::Buffer<float> input(
        static_cast<std::size_t>(std::max(0, shape.num_input_ports)),
        static_cast<std::size_t>(block_size));
    pulp::audio::Buffer<float> output(
        static_cast<std::size_t>(std::max(0, shape.num_output_ports)),
        static_cast<std::size_t>(block_size));

    std::uint32_t noise = 0x9e3779b9u ^ static_cast<std::uint32_t>(id);
    for (std::size_t ch = 0; ch < input.num_channels(); ++ch) {
        auto channel = input.channel(ch);
        for (auto& sample : channel) {
            noise = noise * 1664525u + 1013904223u;
            const auto bits = static_cast<std::uint16_t>((noise >> 8) & 0xffffu);
            const float centered =
                (static_cast<float>(bits) / 32767.5f) - 1.0f;
            sample = centered * 0.25f;
        }
    }

    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi::UmpBuffer ump_in;
    midi::UmpBuffer ump_out;
    prepare_midi_block_storage(midi_in, ump_in);
    prepare_midi_block_storage(midi_out, ump_out);
    ParameterEventQueue param_events;
    pulp::audio::AudioProcessLoadMeasurer warm;

    auto output_view = output.view();
    const auto& const_input = input;
    auto input_view = const_input.view();
    std::array<format::ProcessBusBufferView<const float>, 1> input_buses{{
        {
            .info = {
                .name = "Plugin Node In",
                .index = 0,
                .direction = format::BusDirection::Input,
                .role = format::BusRole::Main,
                .declared_channels = static_cast<int>(input_view.num_channels()),
                .optional = input_view.num_channels() == 0,
                .active = input_view.num_channels() > 0,
            },
            .buffer = input_view,
        },
    }};
    std::array<format::ProcessBusBufferView<float>, 1> output_buses{{
        {
            .info = {
                .name = "Plugin Node Out",
                .index = 0,
                .direction = format::BusDirection::Output,
                .role = format::BusRole::Main,
                .declared_channels = static_cast<int>(output_view.num_channels()),
                .optional = false,
                .active = output_view.num_channels() > 0,
            },
            .buffer = output_view,
        },
    }};
    format::ProcessBuffers process_buffers{
        format::ProcessBusBufferSet<const float>{std::span(input_buses)},
        format::ProcessBusBufferSet<float>{std::span(output_buses)},
    };

    for (std::uint64_t i = 0; i < kLiveSwapWarmBlocks; ++i) {
        output.clear();
        midi_out.clear();
        midi_out.clear_sysex();
        if (auto* ump = midi_out.ump()) ump->clear();
        warm.begin(block_size, sample_rate);
        slot.process(process_buffers, midi_in, midi_out, param_events, block_size);
        warm.end();
    }
    return warm.snapshot();
}

bool SignalGraph::node_has_feedback_edge_locked_(NodeId id) const {
    assert_graph_mutation_locked_();
    for (const auto& c : connections_) {
        if (c.feedback && (c.source_node == id || c.dest_node == id)) return true;
    }
    return false;
}

bool SignalGraph::node_has_parameter_or_automation_contract_locked_(
    NodeId id) const {
    assert_graph_mutation_locked_();
    if (auto it = prepared_plugin_meta_.find(id);
        it != prepared_plugin_meta_.end() && !it->second.parameters.empty()) {
        return true;
    }
    for (const auto& c : connections_) {
        if (c.dest_node == id && (c.automation || c.audio_rate_modulation)) {
            return true;
        }
    }
    return false;
}

bool SignalGraph::staged_replacement_relaxes_identity_locked_(NodeId id) const {
    assert_graph_mutation_locked_();
    return staged_replacements_.find(id) != staged_replacements_.end();
}

void SignalGraph::set_live_swap_diagnostics_locked_(
    LiveSwapFallbackReason reason,
    NodeId node,
    std::string message) {
    assert_graph_mutation_locked_();
    last_swap_diagnostics_ = LiveSwapDiagnostics{
        .reason = reason,
        .offending_node = node,
        .message = std::move(message),
    };
    if (reason != LiveSwapFallbackReason::None) {
        runtime::log_info("SignalGraph: live plugin swap refused for node {}: {} ({})",
                          node,
                          live_swap_reason_name(reason),
                          last_swap_diagnostics_.message);
    }
}

void SignalGraph::cancel_swap_edit_locked_() {
    assert_graph_mutation_locked_();
    in_swap_edit_ = false;
    swap_edit_owner_ = {};
    staged_replacements_.clear();
}

SignalGraph::SwapResult SignalGraph::fail_swap_edit_locked_(
    LiveSwapFallbackReason reason,
    NodeId node,
    std::string message) {
    assert_graph_mutation_locked_();
    set_live_swap_diagnostics_locked_(reason, node, std::move(message));
    cancel_swap_edit_locked_();
    invalidate_live_locked_();
    return SwapResult::NeedsEagerPrepare;
}

std::vector<pulp::audio::AudioProcessLoadSnapshot>
SignalGraph::staged_node_loads_locked_() const {
    assert_graph_mutation_locked_();
    std::vector<pulp::audio::AudioProcessLoadSnapshot> loads;
    loads.reserve(staged_replacements_.size());
    for (const auto& [_, staged] : staged_replacements_) {
        loads.push_back(staged.warmed_load);
    }
    return loads;
}

void SignalGraph::begin_swap_edit() {
    GraphMutationLock lock(*this);
    if (in_swap_edit_) {
        runtime::log_error("SignalGraph: begin_swap_edit() while a swap edit is "
                           "already open — refused (nesting unsupported)");
        return;
    }
    in_swap_edit_ = true;
    ++authoring_generation_;
    swap_edit_owner_ = std::this_thread::get_id();
    staged_replacements_.clear();
    // Reclaim any prior crossfade: a node whose plugin is a fade-done wrapper is
    // reinstated as the bare new instance, so the next published snapshot drops the
    // wrapper and the retired snapshot frees the old instance on this control thread.
    // (Only mutates the authoring graph; the live snapshot still renders the wrapper
    // until prepare_swap publishes.)
    for (auto& n : nodes_) {
        if (auto* xf = dynamic_cast<CrossfadePluginSlot*>(n.plugin.get());
            xf != nullptr && xf->fade_done()) {
            n.plugin = xf->new_slot();
        }
    }
    set_live_swap_diagnostics_locked_(LiveSwapFallbackReason::None, 0, {});
}

SignalGraph::SwapResult SignalGraph::prepare_swap(double sample_rate,
                                                  int max_block_size) {
    struct ObserverCall {
        NodeId id = 0;
        std::shared_ptr<PluginSlot> old_slot;
        std::shared_ptr<PluginSlot> new_slot;
        NodeLiveSwapPolicy policy;
    };
    std::vector<ObserverCall> observer_calls;

    GraphMutationLock lock(*this);
    if (!in_swap_edit_) return SwapResult::NotInSwapEdit;

    // MidiOutput publication is still owned by the live snapshot. Refuse this
    // lane before the generic failure path invalidates that snapshot: callers
    // must be able to drain a pending note-off with extract_midi() and then use
    // eager prepare(). The authoring edits remain in nodes_/connections_; only
    // the old compiled snapshot stays live until that eager replacement.
    const bool candidate_has_midi_output = std::any_of(
        nodes_.begin(), nodes_.end(),
        [](const GraphNode& node) { return node.type == NodeType::MidiOutput; });
    const auto& live = live_slot_.live();
    const bool live_has_midi_output = live != nullptr && std::any_of(
        live->shapes.begin(), live->shapes.end(),
        [](const auto& entry) { return entry.second.type == NodeType::MidiOutput; });
    if (candidate_has_midi_output || live_has_midi_output) {
        set_live_swap_diagnostics_locked_(
            LiveSwapFallbackReason::PredicateExcluded,
            0,
            "MIDI output has snapshot-local pending egress");
        cancel_swap_edit_locked_();
        return SwapResult::NeedsEagerPrepare;
    }

    auto fail = [&](LiveSwapFallbackReason reason,
                    NodeId node,
                    std::string message) {
        return fail_swap_edit_locked_(reason, node, std::move(message));
    };

    if (!preflight_locked_(max_block_size)) {
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "graph preflight rejected the live swap");
    }

    if (!staged_replacements_.empty()) {
        float headroom_threshold = 1.0f;
        for (const auto& [id, _] : staged_replacements_) {
            if (const auto* n = node(id)) {
                headroom_threshold =
                    std::min(headroom_threshold,
                             n->live_swap_policy.headroom_threshold);
            }
        }
        const auto admission = evaluate_live_swap_admission(
            graph_load(),
            staged_node_loads_locked_(),
            headroom_threshold,
            kLiveSwapMinAdmissionCallbacks);
        if (!admission.admit) {
            const auto reason =
                std::string(admission.reason) == "over budget"
                    ? LiveSwapFallbackReason::OverBudget
                    : LiveSwapFallbackReason::NoLoadHistory;
            return fail(reason,
                        staged_replacements_.begin()->first,
                        admission.reason);
        }
    }

    struct CandidateRestore {
        GraphNode* node = nullptr;
        std::shared_ptr<PluginSlot> old_slot;
        std::shared_ptr<PluginSlot> new_slot;  // bare new instance (not the fade wrapper)
        PluginInfo old_info;
        bool old_transport_sensitive = false;
        bool had_metadata = false;
        PreparedPluginMetadata old_metadata;
    };
    std::vector<CandidateRestore> restore;
    restore.reserve(staged_replacements_.size());

    auto rollback_candidate_view = [&]() {
        for (auto it = restore.rbegin(); it != restore.rend(); ++it) {
            it->node->plugin = std::move(it->old_slot);
            it->node->plugin_info = std::move(it->old_info);
            it->node->transport_sensitive = it->old_transport_sensitive;
            if (it->had_metadata) {
                prepared_plugin_meta_[it->node->id] = std::move(it->old_metadata);
            } else {
                prepared_plugin_meta_.erase(it->node->id);
            }
        }
        restore.clear();
    };

    for (const auto& [id, staged] : staged_replacements_) {
        auto* n = node_mut_locked_(id);
        if (!n || n->type != NodeType::Plugin || !n->plugin || !staged.slot) {
            rollback_candidate_view();
            return fail(LiveSwapFallbackReason::PredicateExcluded,
                        id,
                        "staged replacement no longer matches a plugin node");
        }
        const auto meta_it = prepared_plugin_meta_.find(id);
        if (meta_it == prepared_plugin_meta_.end()) {
            rollback_candidate_view();
            return fail(LiveSwapFallbackReason::PredicateExcluded,
                        id,
                        "prepared plugin metadata is unavailable");
        }

        for (const auto& p : meta_it->second.parameters) {
            staged.slot->set_parameter(p.id, n->plugin->get_parameter(p.id));
        }

        CandidateRestore r;
        r.node = n;
        r.old_slot = n->plugin;
        r.old_info = n->plugin_info;
        r.old_transport_sensitive = n->transport_sensitive;
        r.new_slot = staged.slot;
        r.had_metadata = true;
        r.old_metadata = meta_it->second;
        restore.push_back(std::move(r));

        // Click-free crossfade: when the node opts into a fade and there is an old
        // instance to fade from, publish a CrossfadePluginSlot that renders both for
        // fade_ms and blends old->new. It delegates every non-process call to the new
        // instance, so the executor renders it as an ordinary slot; once the fade
        // completes it stops rendering the old instance, and the old is freed on the
        // control thread when a later edit collapses the wrapper (see
        // begin_swap_edit's fade-done reclaim). fade_ms == 0 keeps the instant switch.
        const int fade_ms = n->live_swap_policy.fade_ms;
        if (fade_ms > 0 && r.old_slot) {
            const auto fade_samples = static_cast<std::size_t>(
                std::max<long long>(0, static_cast<long long>(
                    static_cast<double>(fade_ms) * sample_rate / 1000.0)));
            const auto curve = n->live_swap_policy.curve == LiveSwapCurve::Smoothstep
                                   ? signal::TransitionCurve::Smoothstep
                                   : signal::TransitionCurve::EqualPower;
            n->plugin = std::make_shared<CrossfadePluginSlot>(
                staged.slot, r.old_slot, fade_samples, curve,
                staged.info.num_outputs, max_block_size);
        } else {
            n->plugin = staged.slot;
        }
        n->plugin_info = staged.info;
        prepared_plugin_meta_[id] = staged.metadata;
    }

    if (!live_slot_.live() ||
        !snapshot_is_plugin_reinit_free_locked_(*live_slot_.live(), sample_rate,
                                                max_block_size)) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "graph is not eligible for a live swap");
    }
    auto next = compile_(sample_rate, max_block_size, CompileMode::SwapNoAnticipation);
    if (!next) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "candidate graph did not compile");
    }
    // A latency-changing edit still needs the eager/crossfade path: adopting
    // delay history across a host-PDC change would splice different timelines.
    // Feedback is likewise excluded because its previous-block state is local
    // to each executor snapshot and is not part of feed-forward PDC carry.
    auto& old = *live_slot_.live();
    const auto has_feedback = [](const CompiledGraph& graph) {
        return std::any_of(graph.connections.begin(), graph.connections.end(),
                           [](const Connection& c) { return c.feedback; });
    };
    if (next->total_latency_samples != old.total_latency_samples ||
        has_feedback(*next) || has_feedback(old)) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "candidate graph changes latency or carries feedback state");
    }

    // The routed execution domain used before and after publication must stay
    // identical. Otherwise adopting only the legacy state (or only one routed
    // pool) could restart a different execution domain at zero on the next
    // block. The serial and parallel pools intentionally carry independent
    // histories because only one is advanced for any given block.
    if (next->routed.serial.valid != old.routed.serial.valid ||
        next->routed.parallel.valid != old.routed.parallel.valid ||
        next->pdc_execution_domain != old.pdc_execution_domain) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "candidate graph changes routed execution availability or PDC domain");
    }

    // Build an identity-keyed bijection for every live PDC ring. Public
    // Connection values are intentionally insufficient: disconnect+reconnect of
    // an equal-looking edge mints a new private identity and must start clean.
    // Only immutable identity/delay metadata is inspected while audio runs; the
    // ring samples and write cursors remain audio-thread-owned and are shared,
    // never copied.
    if (old.connection_identities.size() != old.connection_delays.size() ||
        next->connection_identities.size() != next->connection_delays.size()) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "candidate graph has inconsistent connection identities");
    }
    std::unordered_map<std::uint64_t, std::size_t> old_delayed;
    for (std::size_t i = 0; i < old.connection_delays.size(); ++i) {
        if (old.connection_delays[i].delay_samples > 0) {
            old_delayed.emplace(old.connection_identities[i], i);
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> matched_delays;
    matched_delays.reserve(old_delayed.size());
    bool delay_structure_ok = true;
    for (std::size_t i = 0; i < next->connection_delays.size(); ++i) {
        const auto& candidate_delay = next->connection_delays[i];
        if (candidate_delay.delay_samples <= 0) continue;
        const auto found = old_delayed.find(next->connection_identities[i]);
        if (found == old_delayed.end()) {
            delay_structure_ok = false;
            break;
        }
        const auto old_index = found->second;
        const auto& live_delay = old.connection_delays[old_index];
        if (candidate_delay.delay_samples != live_delay.delay_samples ||
            !candidate_delay.state || !live_delay.state ||
            candidate_delay.state->ring.size() != live_delay.state->ring.size()) {
            delay_structure_ok = false;
            break;
        }
        matched_delays.emplace_back(i, old_index);
    }
    if (!delay_structure_ok || matched_delays.size() != old_delayed.size()) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "candidate graph changes PDC connection identity or delay");
    }

    const auto routed_can_adopt = [&](const CompiledGraph::RoutedPath& candidate,
                                      const CompiledGraph::RoutedPath& live) {
        if (!candidate.valid) return true;
        for (const auto [candidate_index, live_index] : matched_delays) {
            if (!candidate.pool.can_adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), live.pool,
                    static_cast<std::uint32_t>(live_index))) {
                return false;
            }
        }
        return true;
    };
    if (!routed_can_adopt(next->routed.serial, old.routed.serial) ||
        !routed_can_adopt(next->routed.parallel, old.routed.parallel)) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded,
                    0,
                    "candidate routed pool cannot adopt PDC state");
    }

    for (const auto [candidate_index, live_index] : matched_delays) {
        next->connection_delays[candidate_index].state =
            old.connection_delays[live_index].state;
        if (next->routed.serial.valid) {
            (void)next->routed.serial.pool.adopt_delay_ring_state(
                static_cast<std::uint32_t>(candidate_index), old.routed.serial.pool,
                static_cast<std::uint32_t>(live_index));
        }
        if (next->routed.parallel.valid) {
            (void)next->routed.parallel.pool.adopt_delay_ring_state(
                static_cast<std::uint32_t>(candidate_index), old.routed.parallel.pool,
                static_cast<std::uint32_t>(live_index));
        }
    }

    observer_calls.reserve(staged_replacements_.size());
    for (const auto& r : restore) {
        observer_calls.push_back({
            r.node->id,
            r.old_slot,
            r.new_slot,
            r.node->live_swap_policy,
        });
    }

    // Publish: store the new raw pointer while `next` still owns it, THEN retire
    // the old — never the reverse; seq_cst pairs with process_impl's pinned load so
    // the audio thread never sees a null/torn pointer (NO silent block).
    live_slot_.publish(next);
    total_latency_samples_.store(next->total_latency_samples,
                                 std::memory_order_relaxed);
    publish_prepared_stats_locked_(*next);
    // No reclaim here: Slot::publish() already parked the displaced snapshot and
    // reclaimed it if the readers had drained.
    set_live_swap_diagnostics_locked_(LiveSwapFallbackReason::None, 0, {});
    cancel_swap_edit_locked_();
    lock.unlock();

    for (auto& call : observer_calls) {
        if (call.policy.on_instance_swapped) {
            call.policy.on_instance_swapped(call.id,
                                            std::move(call.old_slot),
                                            std::move(call.new_slot));
        }
    }
    return SwapResult::Swapped;
}

void SignalGraph::abort_swap_edit() {
    GraphMutationLock lock(*this);
    if (!in_swap_edit_) return;
    cancel_swap_edit_locked_();
    invalidate_live_locked_();
}

bool SignalGraph::snapshot_is_plugin_reinit_free_locked_(const CompiledGraph& old_cg,
                                                         double sr, int bs) const {
    assert_graph_mutation_locked_();
    // (1) A sample-rate / block-size change requires re-preparing every instance.
    if (old_cg.sample_rate != sr || old_cg.max_block_size != bs) return false;
    // (2) A custom-type re-register rebinds callbacks to instances the old
    // factory produced (audio-thread type confusion on an opaque void*).
    if (custom_registry_generation_ != old_cg.custom_registry_generation) return false;
    // (3) A swap over a live anticipation pump races the interior instances
    // and the new lane's ring starts empty; exclude anticipation on either side.
    if (anticipation_enabled_.load(std::memory_order_seq_cst) ||
        old_cg.anticipation.valid) {
        return false;
    }
    // (4) A smoothed sparse-automation edge would snap its destination mid-ramp
    // when the fresh snapshot re-primes the slew. Ingress-only MIDI needs no
    // exclusion: routing scratch is block-local, while a stable MidiInput's
    // mailbox and consumed-sequence state are shared. MidiOutput remains
    // snapshot-local, however, so swapping could discard an already-published
    // note-off before extract_midi() observes it; keep output-bearing topologies
    // on eager prepare until their egress mailbox can also be shared safely.
    for (const auto& n : nodes_) {
        if (n.type == NodeType::MidiOutput) return false;
    }
    for (const auto& [_, shape] : old_cg.shapes) {
        if (shape.type == NodeType::MidiOutput) return false;
    }
    for (const auto& c : connections_) {
        if (c.feedback) return false;
        if (c.automation && c.automation_smoothing_ms > 0.0f) return false;
    }
    for (const auto& c : old_cg.connections) {
        if (c.feedback) return false;
    }
    // (5) Identical node SET (no node added / removed / re-typed since this
    // snapshot compiled; cg->shapes holds every node id) plus per-node plugin /
    // custom instance identity, and (6) release-mode cache coverage.
    if (nodes_.size() != old_cg.shapes.size()) return false;
    for (const auto& n : nodes_) {
        if (old_cg.shapes.find(n.id) == old_cg.shapes.end()) return false;
        if (n.type == NodeType::Plugin) {
            const auto it = old_cg.plugins.find(n.id);
            const bool was_resolved = (it != old_cg.plugins.end());
            const bool now_resolved = (n.plugin != nullptr);
            if (was_resolved != now_resolved) return false;  // resolved-since / lost slot
            if (now_resolved) {
                if (it->second.get() != n.plugin.get()
                    && !staged_replacement_relaxes_identity_locked_(n.id)) {
                    return false;
                }
                // A cache miss would silently yield empty params / 0 latency.
                if (prepared_plugin_meta_.find(n.id) == prepared_plugin_meta_.end()) {
                    return false;
                }
            }
        } else if (n.type == NodeType::Custom) {
            if (n.custom_state_pending) return false;  // a pending blob needs a re-prepare
            const auto it = old_cg.custom_instances.find(n.id);
            if (it == old_cg.custom_instances.end()) return false;
            if (it->second != n.custom_instance.get()) return false;  // re-instanced
        }
    }
    return true;
}

} // namespace pulp::host
