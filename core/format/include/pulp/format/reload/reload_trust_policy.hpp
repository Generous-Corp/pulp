#pragma once

// "Require signed reloads" policy for the reload shell and its developer watcher.
// While the dev watcher is compiled in, the policy defaults off and the dev loop
// loads whatever dylib appears (zero friction, the whole point of local
// iteration); a collaborative/open project that holds a signing key can flip it
// on so a shared dev build refuses unsigned code — the same fail-closed
// verification consumers get, applied in dev. A shell built with the watcher
// compiled out defaults the policy ON for its initial load, so a shipped shell
// only ever loads a signed pack.
//
// This resolves the trust material for a watched logic path: it looks for a signed
// sidecar manifest next to the logic (<logic>.manifest.json) and hands the caller a
// SwapPackTrust to pass into gate_logic_image (which does verify-before-load). Pure
// except for reading the sidecar file; no dlopen here.

#include <pulp/format/reload/reload_transaction.hpp>
#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/runtime/crypto.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pulp::format::reload {

struct ReloadTrustPolicy {
    bool require_signed = false;                    ///< off: the unsigned dev loop
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

/// SHA-256 (hex) of a file's bytes, for confirming a staged copy matches the
/// signed manifest hash. std::nullopt if the file cannot be read.
inline std::optional<std::string> sha256_hex_of_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return std::nullopt;
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return runtime::sha256_hex(bytes);
}

/// Apply @p policy to the WATCHED @p logic_path before it is staged for loading.
/// Returns the signed SHA-256 (hex) the caller must re-hash its staged copy
/// against, an empty string when the policy is off (load without trust), or a
/// fail-closed ReloadOutcome. Verifying the source and then re-hashing the staged
/// bytes closes the verify→copy window: the copy re-reads the watched path, so a
/// writer racing between the check and the copy could otherwise swap in unsigned
/// bytes. No dlopen happens here; the bytes are only read.
inline std::variant<std::string, ReloadOutcome>
signed_member_sha256(const std::filesystem::path& logic_path, const ReloadTrustPolicy& policy,
                     ReloadTrustDecision decision) {
    if (!policy.require_signed)
        return std::string{};
    if (auto* refused = std::get_if<ReloadOutcome>(&decision))
        return *refused;
    auto* trust = std::get_if<SwapPackTrust>(&decision);
    if (trust == nullptr) {
        // require_signed with no trust material is a refusal, never an unsigned
        // load: an empty digest would read as "policy off" downstream.
        ReloadOutcome no_trust;
        no_trust.status = ReloadOutcome::Status::RejectedIntegrity;
        no_trust.detail = "require_signed: trust resolution produced no signed pack";
        return no_trust;
    }
    if (auto rejected = verify_pack_before_load(*trust, logic_path.string()))
        return *rejected;
    const std::string member = logic_path.filename().string();
    for (const auto& f : trust->manifest.files)
        if (f.path == member)
            return f.sha256_hex;
    ReloadOutcome not_a_member;
    not_a_member.status = ReloadOutcome::Status::RejectedIntegrity;
    not_a_member.detail = "require_signed: watched file is not a named member of its "
                          "signed manifest";
    return not_a_member;
}

/// The same check with the trust decision resolved from @p logic_path's sidecar.
inline std::variant<std::string, ReloadOutcome>
signed_member_sha256(const std::filesystem::path& logic_path, const ReloadTrustPolicy& policy) {
    if (!policy.require_signed)
        return std::string{};
    return signed_member_sha256(logic_path, policy, resolve_reload_trust(logic_path, policy));
}

}  // namespace pulp::format::reload
