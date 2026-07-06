#pragma once

// Opt-in "require signed reloads" policy for the developer reload watcher. By
// default the dev loop loads whatever dylib appears (zero friction, the whole point
// of local iteration). A collaborative/open project that holds a signing key can
// flip this on so a shared dev build refuses unsigned code — the same fail-closed
// verification consumers get, applied in dev.
//
// This resolves the trust material for a watched logic path: it looks for a signed
// sidecar manifest next to the logic (<logic>.manifest.json) and hands the caller a
// SwapPackTrust to pass into gate_logic_image (which does verify-before-load). Pure
// except for reading the sidecar file; no dlopen here.

#include <pulp/format/reload/reload_transaction.hpp>
#include <pulp/format/reload/swap_pack.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace pulp::format::reload {

struct ReloadTrustPolicy {
    bool require_signed = false;                    ///< default OFF (unsigned dev loop)
    std::vector<std::uint8_t> trusted_public_key;   ///< the pinned signer, when required
};

/// Sidecar manifest path for a watched logic file.
inline std::filesystem::path reload_sidecar_manifest_path(const std::filesystem::path& logic) {
    return logic.string() + ".manifest.json";
}

/// The three outcomes of applying the policy to @p logic_path:
///  - monostate     → load without trust (policy off; existing dev behavior).
///  - SwapPackTrust  → verify against this before loading (pass to gate_logic_image).
///  - ReloadOutcome  → refuse (policy on but no valid signed sidecar).
using ReloadTrustDecision = std::variant<std::monostate, SwapPackTrust, ReloadOutcome>;

inline ReloadTrustDecision resolve_reload_trust(const std::filesystem::path& logic_path,
                                                const ReloadTrustPolicy& policy) {
    if (!policy.require_signed) return std::monostate{};

    const std::filesystem::path sidecar = reload_sidecar_manifest_path(logic_path);
    std::error_code ec;
    if (!std::filesystem::exists(sidecar, ec) || ec) {
        return ReloadOutcome{ReloadOutcome::Status::RejectedSignature,
                             "require_signed: no signed pack manifest at " + sidecar.string()};
    }
    std::ifstream in(sidecar, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    auto manifest = parse_swap_pack_manifest(json, err);
    if (!manifest) {
        return ReloadOutcome{ReloadOutcome::Status::RejectedSignature,
                             "require_signed: unreadable manifest " + sidecar.string() + ": " + err};
    }
    // The pack root is the logic's directory; gate_logic_image verifies the manifest
    // signature + that logic_path is a member with matching bytes.
    return SwapPackTrust{logic_path.parent_path(), std::move(*manifest),
                         policy.trusted_public_key, /*srl=*/nullptr};
}

}  // namespace pulp::format::reload
