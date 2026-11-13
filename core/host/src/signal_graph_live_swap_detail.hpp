// Private helpers shared by the live-swap transaction translation units.
//
// Keeping identity, contract, and policy normalization here lets the catalog
// and transaction entry points have separate owners without exposing any new
// host API or duplicating the compatibility rules.
#pragma once

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::host::live_swap_detail {

inline constexpr int kLiveSwapMinFadeMs = 5;
inline constexpr int kLiveSwapMaxFadeMs = 200;

inline NodeLiveSwapPolicy clamp_policy(NodeLiveSwapPolicy policy) {
    if (policy.fade_ms != 0)
        policy.fade_ms = std::clamp(policy.fade_ms, kLiveSwapMinFadeMs, kLiveSwapMaxFadeMs);
    policy.headroom_threshold = std::clamp(policy.headroom_threshold, 0.0f, 1.0f);
    return policy;
}

inline std::string normalized_plugin_path(const std::string& path) {
    if (path.empty())
        return {};
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    const fs::path weak = fs::weakly_canonical(p, ec);
    if (!ec)
        return weak.string();
    ec.clear();
    const fs::path abs = fs::absolute(p, ec);
    if (!ec)
        return abs.lexically_normal().string();
    return p.lexically_normal().string();
}

inline std::string plugin_identity_key(const PluginInfo& info) {
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

inline bool same_plugin_identity(const PluginInfo& a, const PluginInfo& b) {
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

inline std::vector<ComparableParam> comparable_params(std::vector<HostParamInfo> params) {
    std::vector<ComparableParam> out;
    out.reserve(params.size());
    for (const auto& p : params)
        out.push_back({p.id, p.min_value, p.max_value, p.default_value, p.flags, p.rate});
    std::sort(out.begin(), out.end(),
              [](const ComparableParam& a, const ComparableParam& b) { return a.id < b.id; });
    return out;
}

inline bool same_param_flags(const ParamFlags& a, const ParamFlags& b) {
    return a.automatable == b.automatable && a.read_only == b.read_only && a.hidden == b.hidden &&
           a.stepped == b.stepped && a.is_bypass == b.is_bypass && a.rampable == b.rampable &&
           a.modulatable == b.modulatable;
}

inline bool same_parameter_contract(std::vector<HostParamInfo> a, std::vector<HostParamInfo> b) {
    auto ca = comparable_params(std::move(a));
    auto cb = comparable_params(std::move(b));
    if (ca.size() != cb.size())
        return false;
    for (std::size_t i = 0; i < ca.size(); ++i) {
        if (ca[i].id != cb[i].id || ca[i].min_value != cb[i].min_value ||
            ca[i].max_value != cb[i].max_value || ca[i].default_value != cb[i].default_value ||
            ca[i].rate != cb[i].rate || !same_param_flags(ca[i].flags, cb[i].flags))
            return false;
    }
    return true;
}

inline const char* reason_name(LiveSwapFallbackReason reason) {
    switch (reason) {
    case LiveSwapFallbackReason::None:
        return "None";
    case LiveSwapFallbackReason::NotOptedIn:
        return "NotOptedIn";
    case LiveSwapFallbackReason::LoadFailed:
        return "LoadFailed";
    case LiveSwapFallbackReason::PrepareFailed:
        return "PrepareFailed";
    case LiveSwapFallbackReason::StateRestoreFailed:
        return "StateRestoreFailed";
    case LiveSwapFallbackReason::HistoryRefused:
        return "HistoryRefused";
    case LiveSwapFallbackReason::StateTooLarge:
        return "StateTooLarge";
    case LiveSwapFallbackReason::ShapeMismatch:
        return "ShapeMismatch";
    case LiveSwapFallbackReason::LatencyChanged:
        return "LatencyChanged";
    case LiveSwapFallbackReason::EditorOpen:
        return "EditorOpen";
    case LiveSwapFallbackReason::ParamContractMismatch:
        return "ParamContractMismatch";
    case LiveSwapFallbackReason::FeedbackNotSwappable:
        return "FeedbackNotSwappable";
    case LiveSwapFallbackReason::OverBudget:
        return "OverBudget";
    case LiveSwapFallbackReason::NoLoadHistory:
        return "NoLoadHistory";
    case LiveSwapFallbackReason::PredicateExcluded:
        return "PredicateExcluded";
    case LiveSwapFallbackReason::UntrustedIdentity:
        return "UntrustedIdentity";
    }
    return "Unknown";
}

// Post-compile half of a no-silence publication shared by prepare_swap() and
// isolated-node recovery. `next` is a freshly compiled candidate and `old` the
// live snapshot it replaces. Returns nullptr after `next` has adopted every live
// feed-forward PDC ring (rings are shared, never copied, so siblings keep their
// delay history bit-exact), or the refusal reason with `next` left unadopted.
// Templated on the snapshot type so this private helper needs no header slot.
template <class Snapshot>
const char* adopt_live_snapshot_state(Snapshot& next, const Snapshot& old) {
    // A latency-changing edit still needs the eager/crossfade path: adopting
    // delay history across a host-PDC change would splice different timelines.
    // Feedback is likewise excluded because its previous-block state is local
    // to each executor snapshot and is not part of feed-forward PDC carry.
    const auto has_feedback = [](const Snapshot& graph) {
        return std::any_of(graph.connections.begin(), graph.connections.end(),
                           [](const auto& c) { return c.feedback; });
    };
    if (next.total_latency_samples != old.total_latency_samples || has_feedback(next) ||
        has_feedback(old))
        return "candidate graph changes latency or carries feedback state";

    // The routed execution domain used before and after publication must stay
    // identical. Otherwise adopting only the legacy state (or only one routed
    // pool) could restart a different execution domain at zero on the next
    // block. The serial and parallel pools intentionally carry independent
    // histories because only one is advanced for any given block.
    if (next.routed.serial.valid != old.routed.serial.valid ||
        next.routed.parallel.valid != old.routed.parallel.valid ||
        next.pdc_execution_domain != old.pdc_execution_domain)
        return "candidate graph changes routed execution availability or PDC domain";

    // Build an identity-keyed bijection for every live PDC ring. Public
    // Connection values are intentionally insufficient: disconnect+reconnect of
    // an equal-looking edge mints a new private identity and must start clean.
    // Only immutable identity/delay metadata is inspected while audio runs; the
    // ring samples and write cursors remain audio-thread-owned and are shared,
    // never copied.
    if (old.connection_identities.size() != old.connection_delays.size() ||
        next.connection_identities.size() != next.connection_delays.size())
        return "candidate graph has inconsistent connection identities";
    std::unordered_map<std::uint64_t, std::size_t> old_delayed;
    for (std::size_t i = 0; i < old.connection_delays.size(); ++i) {
        if (old.connection_delays[i].delay_samples > 0)
            old_delayed.emplace(old.connection_identities[i], i);
    }
    std::vector<std::pair<std::size_t, std::size_t>> matched_delays;
    matched_delays.reserve(old_delayed.size());
    for (std::size_t i = 0; i < next.connection_delays.size(); ++i) {
        const auto& candidate_delay = next.connection_delays[i];
        if (candidate_delay.delay_samples <= 0)
            continue;
        const auto found = old_delayed.find(next.connection_identities[i]);
        if (found == old_delayed.end())
            return "candidate graph changes PDC connection identity or delay";
        const auto& live_delay = old.connection_delays[found->second];
        if (candidate_delay.delay_samples != live_delay.delay_samples || !candidate_delay.state ||
            !live_delay.state ||
            candidate_delay.state->ring.size() != live_delay.state->ring.size())
            return "candidate graph changes PDC connection identity or delay";
        matched_delays.emplace_back(i, found->second);
    }
    if (matched_delays.size() != old_delayed.size())
        return "candidate graph changes PDC connection identity or delay";

    const auto routed_can_adopt = [&](const auto& candidate, const auto& live) {
        if (!candidate.valid)
            return true;
        for (const auto [candidate_index, live_index] : matched_delays) {
            if (!candidate.pool.can_adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), live.pool,
                    static_cast<std::uint32_t>(live_index)))
                return false;
        }
        return true;
    };
    if (!routed_can_adopt(next.routed.serial, old.routed.serial) ||
        !routed_can_adopt(next.routed.parallel, old.routed.parallel))
        return "candidate routed pool cannot adopt PDC state";

    for (const auto [candidate_index, live_index] : matched_delays) {
        next.connection_delays[candidate_index].state = old.connection_delays[live_index].state;
        if (next.routed.serial.valid) {
            (void)next.routed.serial.pool.adopt_delay_ring_state(
                static_cast<std::uint32_t>(candidate_index), old.routed.serial.pool,
                static_cast<std::uint32_t>(live_index));
        }
        if (next.routed.parallel.valid) {
            (void)next.routed.parallel.pool.adopt_delay_ring_state(
                static_cast<std::uint32_t>(candidate_index), old.routed.parallel.pool,
                static_cast<std::uint32_t>(live_index));
        }
    }
    return nullptr;
}

} // namespace pulp::host::live_swap_detail
