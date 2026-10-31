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

} // namespace pulp::host::live_swap_detail
