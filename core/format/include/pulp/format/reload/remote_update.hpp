#pragma once

// Decide whether a remotely-fetched UX update may be applied. This is the pure,
// fail-closed heart of the opt-in remote-update path (Apple-style signed
// auto-update): the network layer downloads a pack, and this function decides
// accept/reject from the bytes + policy alone — no I/O here. Every check is
// fail-closed and the signature is the trust anchor (a hostile or MITM'd host still
// cannot forge a signed pack).
//
// The remote lane carries UX ONLY (ui-script) — native/DSP delivery is a separate,
// stricter path and must never ride this channel.

#include <pulp/format/reload/revocation.hpp>
#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/view/reload_capabilities.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pulp::format::reload {

enum class RemoteUpdateVerdict {
    Accept = 0,
    RejectSignature,             ///< bad signature or integrity (untrusted/tampered)
    RejectKind,                  ///< not a UX (ui-script) pack — wrong lane
    RejectStaleRevocation,       ///< no fresh revocation info for a remote fetch
    RejectRevoked,               ///< signer key or artifact is on the revocation list
    RejectDowngrade,             ///< pack_version <= the installed version
    RejectCapabilityEscalation,  ///< requests a capability the host did not grant
};

struct RemoteUpdateResult {
    RemoteUpdateVerdict verdict = RemoteUpdateVerdict::Accept;
    std::string detail;
    bool ok() const { return verdict == RemoteUpdateVerdict::Accept; }
};

/// @param srl_fresh  true only if the caller obtained/confirmed a current revocation
///        list for THIS fetch. Remote updates fail closed when it is false — a
///        withheld SRL must never let a since-revoked pack slip through.
/// @param srl        the fresh revocation list (may be null only when srl_fresh is
///        false, which already rejects).
inline RemoteUpdateResult evaluate_remote_ux_update(
    const std::filesystem::path& pack_root, const SwapPackManifest& m,
    const std::vector<std::uint8_t>& trusted_public_key,
    std::uint64_t installed_pack_version, const view::CapabilitySet& host_granted,
    bool srl_fresh, const SignedRevocationList* srl) {
    // 1. Authenticity first — nothing else means anything without it.
    if (auto v = verify_swap_pack(pack_root, m, trusted_public_key); !v.ok())
        return {RemoteUpdateVerdict::RejectSignature, v.detail};

    // 2. This channel carries UX only.
    if (m.pack_type != SwapPackKind::UiScript)
        return {RemoteUpdateVerdict::RejectKind,
                "remote update channel accepts ui-script packs only"};

    // 3. Revocation must be FRESH for a remote fetch (fail closed), then honored.
    if (!srl_fresh || srl == nullptr)
        return {RemoteUpdateVerdict::RejectStaleRevocation,
                "no fresh revocation list for this remote fetch"};
    const std::string signer_fpr = srl_hex_encode(m.signer_public_key);
    if (is_revoked(*srl, signer_fpr, /*artifact_hash=*/""))
        return {RemoteUpdateVerdict::RejectRevoked, "signer key is revoked"};
    for (const auto& f : m.files)
        if (is_revoked(*srl, /*signer=*/"", f.sha256_hex))
            return {RemoteUpdateVerdict::RejectRevoked, "artifact is revoked"};

    // 4. No downgrades (a since-fixed-vulnerable version must not replace a newer one).
    if (m.pack_version <= installed_pack_version)
        return {RemoteUpdateVerdict::RejectDowngrade,
                "pack version " + std::to_string(m.pack_version) +
                    " does not exceed installed " + std::to_string(installed_pack_version)};

    // 5. A pushed UX may not request more than the installed plugin was granted.
    view::CapabilitySet requested;
    if (!view::CapabilitySet::parse(m.declared_capabilities, requested))
        return {RemoteUpdateVerdict::RejectCapabilityEscalation,
                "declared capabilities include an unknown token"};
    for (auto c : {view::ReloadCapability::Exec, view::ReloadCapability::Clipboard,
                   view::ReloadCapability::Filesystem, view::ReloadCapability::Storage,
                   view::ReloadCapability::Ai, view::ReloadCapability::RuntimeImport,
                   view::ReloadCapability::Network}) {
        if (requested.has(c) && !host_granted.has(c))
            return {RemoteUpdateVerdict::RejectCapabilityEscalation,
                    "pushed UX requests capability '" + std::string(view::capability_name(c)) +
                        "' the host did not grant"};
    }

    return {RemoteUpdateVerdict::Accept, ""};
}

}  // namespace pulp::format::reload
