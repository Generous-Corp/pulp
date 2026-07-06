#pragma once

// Infer the capabilities a scripted UI needs by scanning its JS for the effectful
// bridge functions it calls. This backs an automatic capability declaration: the
// developer writes normal UI code and the tooling fills in the granted set, so
// least-privilege costs them nothing. It is a conservative token scan, not a full
// parser — it maps each effectful bridge function name to the capability that gates
// its group (kept in lockstep with the bridge's group gating, so a UI that calls a
// gated function always declares the capability that function needs).
//
// A pure-UI call (canvas/layout/widgets/style) maps to nothing. Missing a call here
// is not a security hole — the bridge still refuses an ungranted function at load;
// the developer just sees a clear "declare X" error. Over-declaring is the safe
// direction, so a mixed group maps every one of its functions to the group's
// capability.

#include <pulp/view/reload_capabilities.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::view {

/// Effectful bridge function name → the capability that gates it. Mirrors the
/// bridge's register-group gating; new effectful functions must be added here too.
inline const std::vector<std::pair<std::string_view, ReloadCapability>>&
autocaps_function_table() {
    static const std::vector<std::pair<std::string_view, ReloadCapability>> table = {
        {"exec", ReloadCapability::Exec},
        {"execAsync", ReloadCapability::Exec},
        {"setAICli", ReloadCapability::Ai},
        {"getAICli", ReloadCapability::Ai},
        {"readClipboard", ReloadCapability::Clipboard},
        {"writeClipboard", ReloadCapability::Clipboard},
        {"showOpenDialog", ReloadCapability::Filesystem},
        {"showSaveDialog", ReloadCapability::Filesystem},
        {"chooseFolder", ReloadCapability::Filesystem},
        {"setImageSource", ReloadCapability::Filesystem},
        {"setKnobSpriteStrip", ReloadCapability::Filesystem},
        {"setKnobSpriteCore", ReloadCapability::Filesystem},
        {"setFaderSkin", ReloadCapability::Filesystem},
        {"setFaderTrackWidth", ReloadCapability::Filesystem},
        {"setFaderTrackBorder", ReloadCapability::Filesystem},
        {"setMeterColors", ReloadCapability::Filesystem},
        {"setMeterBarRatio", ReloadCapability::Filesystem},
        {"__loadAssetSync__", ReloadCapability::Filesystem},
        {"loadFont", ReloadCapability::Filesystem},
        {"registerFont", ReloadCapability::Filesystem},
        {"storageGetItem", ReloadCapability::Storage},
        {"storageSetItem", ReloadCapability::Storage},
        {"storageRemoveItem", ReloadCapability::Storage},
        {"setWidgetSchema", ReloadCapability::Storage},
        {"setWidgetLottie", ReloadCapability::Storage},
        {"seekWidgetLottie", ReloadCapability::Storage},
        {"clearWidgetSchema", ReloadCapability::Storage},
        {"saveStylePreset", ReloadCapability::Storage},
        {"loadStylePreset", ReloadCapability::Storage},
        {"__pulpRuntimeImport__", ReloadCapability::RuntimeImport},
        {"__pulpRuntimeSettle__", ReloadCapability::RuntimeImport},
    };
    return table;
}

namespace detail {
inline bool is_js_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}
/// Whole-token match: `name` occurs bounded by non-identifier chars (so `exec`
/// matches `pulp.exec(` but not `execute` or `myExec`).
inline bool contains_call_token(std::string_view src, std::string_view name) {
    std::size_t pos = 0;
    while ((pos = src.find(name, pos)) != std::string_view::npos) {
        const bool left_ok = pos == 0 || !is_js_ident_char(src[pos - 1]);
        const std::size_t after = pos + name.size();
        const bool right_ok = after >= src.size() || !is_js_ident_char(src[after]);
        if (left_ok && right_ok) return true;
        pos = after;
    }
    return false;
}
}  // namespace detail

/// Scan @p js_source and return the granted set it needs.
inline CapabilitySet infer_capabilities_from_js(std::string_view js_source) {
    CapabilitySet caps;
    for (const auto& [name, cap] : autocaps_function_table())
        if (detail::contains_call_token(js_source, name)) caps.grant(cap);
    return caps;
}

/// The capability NAMES a scripted UI needs, sorted + de-duplicated — ready to drop
/// into a manifest's declared_capabilities.
inline std::vector<std::string> infer_capability_names_from_js(std::string_view js_source) {
    const auto caps = infer_capabilities_from_js(js_source);
    std::vector<std::string> names;
    for (auto c : {ReloadCapability::Exec, ReloadCapability::Clipboard,
                   ReloadCapability::Filesystem, ReloadCapability::Storage,
                   ReloadCapability::Ai, ReloadCapability::RuntimeImport,
                   ReloadCapability::Network}) {
        if (caps.has(c)) names.emplace_back(capability_name(c));
    }
    return names;
}

}  // namespace pulp::view
