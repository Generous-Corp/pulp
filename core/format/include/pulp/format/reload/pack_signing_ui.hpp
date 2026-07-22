#pragma once

// Human-facing helpers for the swap-pack signing flow: consistent key names (so a
// key is easy to find, import, rotate, and revoke), a summary of exactly what a
// signature will cover, and a loud provenance banner for a freshly generated key.
// These are pure string builders — the CLI and any tool surface reuse them so the
// developer sees identical guidance everywhere. No I/O, no crypto here.

#include <pulp/format/reload/swap_pack.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::format::reload {

enum class KeyRole { Signing, Revocation };

inline std::string_view key_role_name(KeyRole r) {
    return r == KeyRole::Signing ? "signing" : "revocation";
}

/// Keychain service id: `pulp.reload.<role>.<plugin_id>`. Listing `pulp.reload.*`
/// finds every Pulp key; role + plugin make the exact item obvious.
inline std::string reload_keychain_service(KeyRole role, std::string_view plugin_id) {
    return "pulp.reload." + std::string(key_role_name(role)) + "." + std::string(plugin_id);
}

/// Keychain account/label carrying the generation and a short fingerprint prefix,
/// e.g. `gen1-ab12cd34`. Old generations keep distinct accounts so a rotation never
/// overwrites the previous key in place (it stays importable during a transition).
inline std::string reload_key_account(int generation, std::string_view fpr_hex) {
    std::string fpr8(fpr_hex.substr(0, std::min<std::size_t>(8, fpr_hex.size())));
    return "gen" + std::to_string(generation) + (fpr8.empty() ? "" : "-" + fpr8);
}

/// Upper-snake, secret-name-safe form of an identifier (non-alnum → `_`).
inline std::string to_secret_token(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        else
            out.push_back('_');
    }
    return out;
}

/// GitHub Actions secret name, same shape as the keychain id but secret-safe:
/// `PULP_RELOAD_<ROLE>_KEY_<PLUGIN>_GEN<N>`.
inline std::string reload_github_secret_name(KeyRole role, std::string_view plugin_id,
                                             int generation) {
    return "PULP_RELOAD_" + to_secret_token(key_role_name(role)) + "_KEY_" +
           to_secret_token(plugin_id) + "_GEN" + std::to_string(generation);
}

/// Normalize a repo string (owner/name, URL, or scp-style; strips `.git` + host) to
/// lowercase `owner/name`, or "" if it has no clean owner/name shape.
inline std::string normalize_github_repo(std::string_view repo) {
    if (repo.empty()) return {};
    std::string r;
    for (char c : repo) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (r.size() > 4 && r.substr(r.size() - 4) == ".git") r.resize(r.size() - 4);
    if (auto at = r.find('@'); at != std::string::npos) r = r.substr(at + 1);      // scp-style
    for (const char* scheme : {"https://", "http://", "ssh://", "git://"}) {
        const std::size_t n = std::string_view(scheme).size();
        if (r.rfind(scheme, 0) == 0) { r = r.substr(n); break; }
    }
    if (auto host = r.find("github.com"); host != std::string::npos) {
        // Host/path separator is '/' for URLs and ':' for scp-style remotes.
        const std::size_t sep = r.find_first_of(":/", host);
        if (sep != std::string::npos) r = r.substr(sep + 1);
    }
    while (!r.empty() && (r.front() == '/' || r.front() == ':')) r.erase(r.begin());
    const std::size_t slash = r.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= r.size()) return {};
    if (r.find('/', slash + 1) != std::string::npos) return {};  // extra path segments
    return r;
}

/// A signing key may be published as a GitHub Actions secret ONLY into a plugin's
/// own repository — never the core Pulp repo, and never an empty/malformed target.
/// Fail closed: when unsure, refuse rather than risk leaking a key into the wrong
/// repo.
inline bool github_backup_repo_allowed(std::string_view repo) {
    const std::string r = normalize_github_repo(repo);
    // Block BOTH core-repo names. The framework moved danielraffel/pulp ->
    // Generous-Corp/pulp; GitHub still redirects the old name to the new repo,
    // so a key targeting EITHER would land in the core repo. Blocking only one
    // leaves a hole — the old name still resolves to core, and the new name IS
    // core. `normalize_github_repo` lowercases, so compare lowercased.
    return !r.empty() && r != "danielraffel/pulp" && r != "generous-corp/pulp";
}

/// A readable summary of everything the signature will cover, so the signer can
/// confirm exactly what they are vouching for before a key touches it.
inline std::string swap_pack_signing_summary(const SwapPackManifest& m) {
    std::string s;
    s += "About to sign swap pack:\n";
    s += "  id:            " + m.id + "\n";
    s += "  plugin:        " + m.plugin_id + "\n";
    s += "  pack version:  " + std::to_string(m.pack_version) + "\n";
    s += "  pack type:     " + std::string(swap_pack_kind_to_string(m.pack_type)) + "\n";
    s += "  update channel:" + (m.update_channel.empty() ? std::string(" (none)")
                                                          : " " + m.update_channel) + "\n";
    s += "  min host:      " + std::to_string(m.min_host_version) + "\n";
    s += "  capabilities:  ";
    if (m.declared_capabilities.empty()) {
        s += "(none — pure UI)";
    } else {
        for (std::size_t i = 0; i < m.declared_capabilities.size(); ++i)
            s += (i ? ", " : "") + m.declared_capabilities[i];
    }
    s += "\n  files:\n";
    for (const auto& f : m.files) {
        const std::string h8 = f.sha256_hex.substr(0, std::min<std::size_t>(8, f.sha256_hex.size()));
        s += "    " + f.path + "  [" + std::string(swap_pack_kind_to_string(f.kind)) +
             "]  " + h8 + "…\n";
    }
    return s;
}

/// The loud, no-surprise banner shown when Pulp generates a key the developer did
/// not already have. Names what was created, where it lives, and the exact
/// consequences — so a key is never a silent surprise.
inline std::string key_generation_banner(KeyRole role, std::string_view keychain_service,
                                         std::string_view plugin_id) {
    const bool signing = role == KeyRole::Signing;
    std::string s;
    s += "════════════════════════════════════════════════════════════════\n";
    s += std::string("  Pulp generated a new ") + std::string(key_role_name(role)) +
         " key for " + std::string(plugin_id) + ".\n";
    s += "  Stored in your keychain as: " + std::string(keychain_service) + "\n";
    s += "  THIS KEY IS YOUR PLUGIN'S IDENTITY — back it up now.\n";
    if (signing)
        s += "  • Lose it and you cannot ship signed updates for this plugin.\n";
    else
        s += "  • Keep it OFFLINE. Lose it and you cannot revoke a leaked signing key.\n";
    s += "  • Leak it and an attacker can sign code as you.\n";
    s += "  Pulp will reuse this key; it never silently generates a second one.\n";
    s += "════════════════════════════════════════════════════════════════\n";
    return s;
}

}  // namespace pulp::format::reload
