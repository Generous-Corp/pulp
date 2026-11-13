// signal_graph_live_swap.cpp — SignalGraph's LIVE-SWAP TRANSACTION ENGINE.
//
// Holds everything behind the no-silence topology edit: the staged-replacement
// pipeline
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
#include <pulp/host/isolated_plugin_slot.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include "signal_graph_internal.hpp"
#include "signal_graph_live_swap_detail.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
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

} // namespace

using namespace live_swap_detail;

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

SignalGraph::SwapResult
SignalGraph::stage_plugin_replacement(NodeId id, PluginCatalogToken token) {
    GraphMutationLock mutation_lock(*this);
    if (refuse_isolated_mode_locked_("stage_plugin_replacement"))
        return SwapResult::NeedsEagerPrepare;

    if (refuse_spectral_mutation_())
        return SwapResult::NeedsEagerPrepare;
    if (!in_swap_edit_)
        return SwapResult::NotInSwapEdit;

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
    {
        auto settings = loaded->host_settings();
        if (settings.process_mode != hosted_plugin_process_mode_) {
            settings.process_mode = hosted_plugin_process_mode_;
            loaded->set_host_settings(settings);
        }
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

    // DSP history is distinct from preset state.  Carry it only for an exact
    // history key; Refuse is the fail-closed mode for cells whose algorithm,
    // sample-rate/latency contract, or state schema cannot be proven equal.
    const auto history_mode = policy.retained_history.mode;
    if (history_mode == RetainedHistoryMode::Reseed) {
        if (!loaded->reseed_dsp_state(policy.retained_history.reseed)) {
            return fail_swap_edit_locked_(LiveSwapFallbackReason::HistoryRefused, id,
                                          "replacement DSP cannot accept the requested reseed");
        }
    } else if (history_mode == RetainedHistoryMode::Adopt ||
               history_mode == RetainedHistoryMode::Crossfade ||
               history_mode == RetainedHistoryMode::Refuse) {
        const auto old_key = n->plugin->retained_history_key();
        const auto new_key = loaded->retained_history_key();
        const bool compatible = !old_key.empty() && old_key == new_key;
        auto dsp_state =
            compatible ? n->plugin->serialize_dsp_state() : std::vector<std::uint8_t>{};
        if (dsp_state.size() > policy.retained_history.max_bytes) {
            return fail_swap_edit_locked_(LiveSwapFallbackReason::HistoryRefused, id,
                                          "retained DSP history exceeds the policy limit");
        }
        const bool restore_failed =
            compatible && !dsp_state.empty() && !loaded->restore_dsp_state(dsp_state);
        if (history_mode == RetainedHistoryMode::Refuse &&
            (!compatible || dsp_state.empty() || restore_failed)) {
            return fail_swap_edit_locked_(
                LiveSwapFallbackReason::HistoryRefused, id,
                "retained DSP history identity or restore is incompatible");
        }
        if (history_mode != RetainedHistoryMode::Refuse && restore_failed) {
            return fail_swap_edit_locked_(LiveSwapFallbackReason::HistoryRefused, id,
                                          "replacement DSP rejected compatible retained history");
        }
    }

    StagedReplacement staged;
    staged.id = id;
    staged.info = loaded_info;
    staged.metadata = PreparedPluginMetadata{
        std::move(new_params),
        new_latency,
        wants_transport,
        loaded->restart_request_count(),
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
        runtime::log_info("SignalGraph: live plugin swap refused for node {}: {} ({})", node,
                          reason_name(reason), last_swap_diagnostics_.message);
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
    if (refuse_isolated_mode_locked_("begin_swap_edit"))
        return;

    if (refuse_spectral_mutation_())
        return;
    if (!trigger_producers_.empty() || !trigger_envelopes_.empty() || !trigger_audio_.empty() ||
        !trigger_edges_.empty())
        return;
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
    if (refuse_isolated_mode_locked_("prepare_swap"))
        return SwapResult::NeedsEagerPrepare;

    if (refuse_spectral_mutation_())
        return SwapResult::NeedsEagerPrepare;
    if (!trigger_producers_.empty() || !trigger_envelopes_.empty() || !trigger_audio_.empty() ||
        !trigger_edges_.empty())
        return SwapResult::NeedsEagerPrepare;
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
    if (const char* refusal = adopt_live_snapshot_state(*next, *live_slot_.live())) {
        rollback_candidate_view();
        return fail(LiveSwapFallbackReason::PredicateExcluded, 0, refusal);
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
    if (refuse_spectral_mutation_())
        return;
    if (!in_swap_edit_)
        return;
    cancel_swap_edit_locked_();
    invalidate_live_locked_();
}

SignalGraph::SwapResult
SignalGraph::publish_isolated_recovery(NodeId id, const PluginSlot& terminal,
                                       std::unique_ptr<PluginSlot> replacement) {
    GraphMutationLock lock(*this);
    // Every refusal leaves the live snapshot published: the terminal node keeps
    // rendering silence and its siblings never lose a block to a failed recovery.
    const auto refuse = [&](LiveSwapFallbackReason reason, const char* message) {
        set_live_swap_diagnostics_locked_(reason, id, message);
        return SwapResult::NeedsEagerPrepare;
    };
    if (refuse_spectral_mutation_())
        return SwapResult::NeedsEagerPrepare;
    if (in_swap_edit_ || !trigger_producers_.empty() || !trigger_envelopes_.empty() ||
        !trigger_audio_.empty() || !trigger_edges_.empty())
        return refuse(LiveSwapFallbackReason::PredicateExcluded,
                      "a swap edit or trigger graph owns publication");
    const auto live = live_slot_.live();
    auto* n = node_mut_locked_(id);
    const auto published = [&] {
        const auto it = live->plugins.find(id);
        return it != live->plugins.end() && it->second.get() == &terminal;
    };
    if (!live || !n || n->type != NodeType::Plugin || n->plugin.get() != &terminal || !published())
        return refuse(LiveSwapFallbackReason::PredicateExcluded,
                      "node does not publish the given isolated slot");
    if (!replacement || !is_isolated_plugin_slot(terminal) ||
        !is_isolated_plugin_slot(*replacement))
        return refuse(LiveSwapFallbackReason::UntrustedIdentity,
                      "recovery replaces only isolated proxies with isolated proxies");
    // An isolated proxy reports unloaded exactly when its fault word is set.
    if (terminal.is_loaded())
        return refuse(LiveSwapFallbackReason::PredicateExcluded, "isolated node is not terminal");
    if (!same_plugin_identity(terminal.info(), replacement->info()))
        return refuse(LiveSwapFallbackReason::UntrustedIdentity,
                      "replacement plugin identity differs from the terminal node");
    const auto meta = prepared_plugin_meta_.find(id);
    const auto shape = live->shapes.find(id);
    if (meta == prepared_plugin_meta_.end() || shape == live->shapes.end() ||
        shape->second.num_input_ports != replacement->info().num_inputs ||
        shape->second.num_output_ports != replacement->info().num_outputs)
        return refuse(LiveSwapFallbackReason::ShapeMismatch,
                      "replacement port shape differs from the live node");
    if (std::max(0, replacement->latency_samples()) != meta->second.latency_samples)
        return refuse(LiveSwapFallbackReason::LatencyChanged,
                      "replacement latency differs from the live node");
    if (!replacement->prepare(live->sample_rate, live->max_block_size))
        return refuse(LiveSwapFallbackReason::PrepareFailed, "replacement did not prepare");

    std::shared_ptr<PluginSlot> fresh(std::move(replacement));
    const auto old_slot = n->plugin;
    const auto old_meta = meta->second;
    n->plugin = fresh;
    meta->second.restart_requests = fresh->restart_request_count();
    StagedReplacement staged;
    staged.id = id;
    staged.info = fresh->info();
    staged.slot = fresh;
    staged.metadata = meta->second;
    staged.same_identity = true;
    // The staged entry only relaxes the reinit-free instance-identity check for
    // this node; it is cleared before the lock is released on every path.
    staged_replacements_[id] = std::move(staged);
    const auto rollback = [&](const char* message) {
        n->plugin = old_slot;
        prepared_plugin_meta_[id] = old_meta;
        staged_replacements_.clear();
        return refuse(LiveSwapFallbackReason::PredicateExcluded, message);
    };
    if (!snapshot_is_plugin_reinit_free_locked_(*live, live->sample_rate, live->max_block_size))
        return rollback("graph is not eligible for a live swap");
    auto next = compile_(live->sample_rate, live->max_block_size, CompileMode::SwapNoAnticipation);
    if (!next)
        return rollback("candidate graph did not compile");
    if (const char* refusal = adopt_live_snapshot_state(*next, *live))
        return rollback(refusal);

    // Same publication order as prepare_swap(): the audio thread pins either the
    // old snapshot (terminal silence) or the new one, never a torn pointer, and the
    // displaced snapshot (with the terminal proxy) is reclaimed on this thread.
    live_slot_.publish(next);
    total_latency_samples_.store(next->total_latency_samples, std::memory_order_relaxed);
    publish_prepared_stats_locked_(*next);
    staged_replacements_.clear();
    set_live_swap_diagnostics_locked_(LiveSwapFallbackReason::None, 0, {});
    return SwapResult::Swapped;
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
    // snapshot compiled; authored_shapes holds every node id before any private
    // sample-region quotienting) plus per-node plugin /
    // custom instance identity, and (6) release-mode cache coverage.
    if (nodes_.size() != old_cg.authored_shapes.size())
        return false;
    for (const auto& n : nodes_) {
        const auto shape = old_cg.authored_shapes.find(n.id);
        if (shape == old_cg.authored_shapes.end() || shape->second.type != n.type ||
            shape->second.num_input_ports != n.num_input_ports ||
            shape->second.num_output_ports != n.num_output_ports) {
            return false;
        }
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
