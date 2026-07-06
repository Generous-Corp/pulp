#pragma once

// Reload capability model (live-swap trust, C2). A signed reload pack declares the
// capabilities its scripted UI needs; the host grants a set; the WidgetBridge is
// constructed capability-scoped so ungranted EFFECTFUL API groups are simply not
// registered (the JS symbol is absent). Pure-UI groups (canvas/css/layout/widgets/
// gpu/…) are always registered and need no capability.
//
// Enforcement seam: the `register_*_api()` groups in WidgetBridge. Native code is
// NOT capability-gated (that's provenance-only, by design). See
// planning/2026-07-05-reload-trust-and-safety-model.md (C2) for the audited
// group→capability mapping and the rationale (signing = provenance, capabilities =
// blast-radius cap).
//
// Home is `view` (not `format`) because the bridge is the enforcement point and
// `format` may include `view`, not the reverse.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

/// A capability gates one effectful bridge-API group. Ungranted → the group's JS
/// functions are absent. Ordering is stable (persisted/signed by name, never index).
enum class ReloadCapability : std::uint8_t {
    Exec,          ///< shell exec/execAsync — ALSO compiled out of shipping builds
    Clipboard,     ///< read/write the system clipboard
    Filesystem,    ///< read arbitrary file paths (asset file loads, native dialogs)
    Storage,       ///< persistent key/value + preset temp files
    Ai,            ///< AI-CLI bridge
    RuntimeImport, ///< evaluate a runtime-imported JS bundle (dynamic code load)
    Network,       ///< outbound network (reserved; used by remote-UX-push, not the
                   ///< bridge today)
};

/// Stable wire name for a capability (used in the SIGNED pack manifest + config).
/// Names are the contract; never serialize the enum index.
inline std::string_view capability_name(ReloadCapability c) {
    switch (c) {
        case ReloadCapability::Exec:          return "exec";
        case ReloadCapability::Clipboard:     return "clipboard";
        case ReloadCapability::Filesystem:    return "filesystem";
        case ReloadCapability::Storage:       return "storage";
        case ReloadCapability::Ai:            return "ai";
        case ReloadCapability::RuntimeImport: return "runtime_import";
        case ReloadCapability::Network:       return "network";
    }
    return "unknown";
}

/// Parse a wire name; returns false on an unknown token (caller fails closed — an
/// unrecognized capability must never silently grant nothing-or-everything).
inline bool capability_from_name(std::string_view s, ReloadCapability& out) {
    if (s == "exec")           { out = ReloadCapability::Exec;          return true; }
    if (s == "clipboard")      { out = ReloadCapability::Clipboard;     return true; }
    if (s == "filesystem")     { out = ReloadCapability::Filesystem;    return true; }
    if (s == "storage")        { out = ReloadCapability::Storage;       return true; }
    if (s == "ai")             { out = ReloadCapability::Ai;            return true; }
    if (s == "runtime_import") { out = ReloadCapability::RuntimeImport; return true; }
    if (s == "network")        { out = ReloadCapability::Network;       return true; }
    return false;
}

/// A granted-capability set. Small fixed universe → a bitmask. Default = EMPTY
/// (most restrictive), which is the consumer/protected-dev default; local-dev
/// constructs `all()` (unenforced).
class CapabilitySet {
public:
    CapabilitySet() = default;

    static CapabilitySet all() {
        CapabilitySet s;
        s.bits_ = kAllBits;
        return s;
    }

    void grant(ReloadCapability c) { bits_ |= bit(c); }
    bool has(ReloadCapability c) const { return (bits_ & bit(c)) != 0; }
    bool empty() const { return bits_ == 0; }

    /// Parse a declared-capability name list (from the signed manifest). Returns
    /// false if any token is unknown (fail closed — reject the whole set).
    static bool parse(const std::vector<std::string>& names, CapabilitySet& out) {
        CapabilitySet s;
        for (const auto& n : names) {
            ReloadCapability c;
            if (!capability_from_name(n, c)) return false;  // unknown → fail closed
            s.grant(c);
        }
        out = s;
        return true;
    }

private:
    static std::uint32_t bit(ReloadCapability c) {
        return std::uint32_t{1} << static_cast<std::uint8_t>(c);
    }
    static constexpr std::uint32_t kAllBits =
        (std::uint32_t{1} << 7) - 1;  // 7 capabilities (Exec..Network)

    std::uint32_t bits_ = 0;
};

}  // namespace pulp::view
