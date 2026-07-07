#pragma once

// Opt-in remote UX update fetch — the network layer over evaluate_remote_ux_update.
//
// Default OFF. When a developer opts in, this fetches a signed UX pack over HTTPS,
// hands the raw bytes to the pure fail-closed gate (remote_update.hpp), and — only
// if the gate accepts — installs it content-addressed (pack_install.hpp), so the
// loader reads immutable, re-verified files. The signature is the trust anchor: a
// hostile or MITM'd host still cannot forge a signed pack.
//
// Two deliberate design properties the owner asked for:
//
//   * Offline is a FIRST-CLASS outcome, never an error. An unreachable host, a
//     timeout, or a non-2xx response yields RemoteFetchStatus::Unavailable — the
//     caller simply keeps its current UI. Nothing here blocks or throws, so a dead
//     network never stalls the plugin. A developer may supply an offline_fallback
//     pack so the plugin still looks like THEIRS with no network.
//
//   * Everything is customizable. The transport is an injected `fetcher` callable,
//     so any delivery "style" works — a CDN, an object store, a private server with
//     custom auth. `make_http_pack_fetcher()` is a convenience default over
//     runtime::http_*; override it freely.
//
// Privacy: an update check sends ONLY what the server needs to choose the right pack
// — plugin id, channel, installed version — over HTTPS. No device id, no user id, no
// telemetry. `build_update_check_url()` is pure so that contract is testable.

#include <pulp/format/reload/pack_install.hpp>
#include <pulp/format/reload/remote_update.hpp>
#include <pulp/format/reload/revocation.hpp>
#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/runtime/http.hpp>
#include <pulp/view/reload_capabilities.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pulp::format::reload {

enum class RemoteFetchStatus {
    Disabled,     ///< opt-in feature is off (default) — no network is touched
    Applied,      ///< a newer signed pack passed the gate and was installed
    UpToDate,     ///< reachable, but nothing newer to apply (server offered <= installed)
    Rejected,     ///< a pack was returned but the gate refused it (a security event)
    Unavailable,  ///< offline / unreachable / timeout / malformed transport — keep current UI
};

struct RemoteFetchResult {
    RemoteFetchStatus status = RemoteFetchStatus::Disabled;
    RemoteUpdateResult decision;              ///< the gate verdict, when a pack was evaluated
    std::filesystem::path installed_root;     ///< Applied: the immutable dir to load from;
                                              ///< Unavailable: the offline fallback, if supplied
    std::string detail;
    bool applied() const { return status == RemoteFetchStatus::Applied; }
};

/// What an injected fetcher returns. `reachable == false` means offline/unreachable
/// (→ Unavailable) — it is NOT an error path; leave the other fields empty.
struct FetchedPack {
    bool reachable = false;
    std::string manifest_json;         ///< the pack manifest as served
    std::filesystem::path local_root;  ///< where the fetcher materialized the pack files
    std::string detail;
};

/// Percent-encode a query value (RFC 3986 unreserved kept; everything else escaped),
/// so a plugin id / channel with odd characters cannot break out of the query.
inline std::string url_query_escape(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

/// The privacy-preserving update-check URL: base + only (plugin, channel, installed
/// version). Nothing identifying is added. Pure + testable.
inline std::string build_update_check_url(const std::string& base_url,
                                          const std::string& plugin_id,
                                          const std::string& channel,
                                          std::uint64_t installed_version) {
    const char sep = (base_url.find('?') == std::string::npos) ? '?' : '&';
    return base_url + sep + "plugin=" + url_query_escape(plugin_id) +
           "&channel=" + url_query_escape(channel) +
           "&installed=" + std::to_string(installed_version);
}

/// True only if @p p is safe to join onto a download directory: a non-empty,
/// relative path with no root name and no `..` component. The manifest is the
/// untrusted server's word (its signature is verified only later), so a fetcher
/// MUST screen every declared file path with this before writing it, or a hostile
/// `../../x` escapes the download directory.
inline bool is_safe_pack_member_path(const std::string& p) {
    if (p.empty()) return false;
    const std::filesystem::path rel(p);
    if (!rel.is_relative() || rel.has_root_name()) return false;
    for (const auto& part : rel)
        if (part == "..") return false;
    return true;
}

struct RemoteUpdateConfig {
    bool enabled = false;                          ///< opt-in; default OFF (no network)
    std::string base_url;                          ///< HTTPS update-check endpoint
    std::string plugin_id;
    std::string channel = "stable";
    std::vector<std::uint8_t> trusted_public_key;  ///< the pinned signer
    std::uint64_t installed_pack_version = 0;
    view::CapabilitySet host_granted;              ///< the ceiling a pushed UX may not exceed
    std::filesystem::path install_base;            ///< where an accepted pack is installed
    /// Optional developer-provided pack to fall back to when offline, so the plugin
    /// keeps THEIR look with no network. Surfaced as installed_root on Unavailable;
    /// never fetched, so it is the developer's own trusted/bundled content.
    std::filesystem::path offline_fallback_root;
    /// Transport. Inject your own (CDN / object store / authed server). When unset,
    /// the fetch cannot proceed and returns Unavailable — call make_http_pack_fetcher()
    /// (or your own) to enable it.
    std::function<FetchedPack(const std::string& url)> fetcher;
};

/// Orchestrate one opt-in remote-update check. Fail-closed + offline-graceful:
/// disabled → no network; non-HTTPS or no fetcher → Unavailable; unreachable →
/// Unavailable (+ optional offline fallback); reachable → parse → gate → (on accept)
/// content-addressed install. @p srl_fresh / @p srl gate revocation freshness exactly
/// as the pure gate requires (a remote fetch fails closed without fresh revocation).
inline RemoteFetchResult check_and_fetch_remote_ux_update(const RemoteUpdateConfig& cfg,
                                                          bool srl_fresh,
                                                          const SignedRevocationList* srl) {
    if (!cfg.enabled)
        return {RemoteFetchStatus::Disabled, {}, {}, "remote update disabled (opt-in)"};

    auto offline = [&](std::string why) {
        RemoteFetchResult r{RemoteFetchStatus::Unavailable, {}, {}, std::move(why)};
        if (!cfg.offline_fallback_root.empty()) {
            r.installed_root = cfg.offline_fallback_root;
            r.detail += " — using offline fallback pack";
        }
        return r;
    };

    // Plaintext update channels are not allowed — the signature is the trust anchor,
    // but HTTPS still protects the request's privacy + prevents trivial tampering.
    if (cfg.base_url.rfind("https://", 0) != 0)
        return offline("remote update requires an https:// endpoint");
    if (!cfg.fetcher) return offline("no fetcher configured");

    const std::string url =
        build_update_check_url(cfg.base_url, cfg.plugin_id, cfg.channel, cfg.installed_pack_version);
    const FetchedPack fetched = cfg.fetcher(url);
    if (!fetched.reachable) return offline("offline: " + fetched.detail);

    std::string err;
    auto manifest = parse_swap_pack_manifest(fetched.manifest_json, err);
    if (!manifest)
        return {RemoteFetchStatus::Rejected,
                {RemoteUpdateVerdict::RejectSignature, "malformed manifest: " + err},
                {}, "malformed manifest: " + err};

    const auto decision =
        evaluate_remote_ux_update(fetched.local_root, *manifest, cfg.trusted_public_key,
                                  cfg.installed_pack_version, cfg.host_granted, srl_fresh, srl);
    if (!decision.ok()) {
        // A server offering an equal/older version is benign "nothing newer" — not a
        // security event. Everything else is a real rejection to surface loudly.
        const bool benign = decision.verdict == RemoteUpdateVerdict::RejectDowngrade;
        return {benign ? RemoteFetchStatus::UpToDate : RemoteFetchStatus::Rejected,
                decision, {}, decision.detail};
    }

    // Accepted: install content-addressed — this re-verifies the exact bytes at the
    // destination, so the loader never reads anything the gate did not clear.
    const auto inst = install_verified_pack(fetched.local_root, *manifest,
                                            cfg.trusted_public_key, cfg.install_base);
    if (!inst.ok())
        return {RemoteFetchStatus::Rejected, decision, {}, "install failed: " + inst.detail};
    return {RemoteFetchStatus::Applied, decision, inst.installed_root, "applied"};
}

/// Convenience default transport over runtime::http_*: GET the manifest, then
/// download each declared file from `<files_base_url>/<relative path>` into
/// @p download_dir. Unreachable/non-2xx → reachable=false (offline). Override with
/// your own fetcher for a different layout, auth, or CDN.
inline std::function<FetchedPack(const std::string&)> make_http_pack_fetcher(
    std::string files_base_url, std::filesystem::path download_dir, int timeout_seconds = 20) {
    return [files_base_url = std::move(files_base_url),
            download_dir = std::move(download_dir), timeout_seconds](const std::string& url) {
        FetchedPack out;
        const auto manifest_resp = runtime::http_get(url, timeout_seconds);
        if (!manifest_resp.ok()) {
            out.detail = manifest_resp.error.empty()
                             ? ("HTTP " + std::to_string(manifest_resp.status_code))
                             : manifest_resp.error;
            return out;  // reachable stays false → Unavailable
        }
        std::string err;
        auto manifest = parse_swap_pack_manifest(manifest_resp.body, err);
        if (!manifest) {
            // Reachable, but the manifest is unusable — let the orchestrator reject it
            // (a served-but-malformed manifest is a server problem, not offline).
            out.reachable = true;
            out.manifest_json = manifest_resp.body;
            return out;
        }
        std::error_code ec;
        std::filesystem::create_directories(download_dir, ec);
        for (const auto& f : manifest->files) {
            // The manifest is the UNTRUSTED server's word (the signature is checked
            // later, by the gate). A hostile `path` must never let this write outside
            // download_dir, so screen it BEFORE opening a destination.
            // (install_verified_pack re-checks at publish time; this guards the
            // download write itself.)
            if (!is_safe_pack_member_path(f.path)) {
                out.detail = "unsafe manifest path rejected: " + f.path;
                return out;  // fail closed — treat as unavailable, write nothing
            }
            const std::filesystem::path dest = download_dir / f.path;
            std::filesystem::create_directories(dest.parent_path(), ec);
            if (!runtime::http_download(files_base_url + "/" + f.path, dest.string(),
                                        timeout_seconds)) {
                out.detail = "download failed: " + f.path;
                return out;  // partial fetch → treat as offline/unavailable
            }
        }
        out.reachable = true;
        out.manifest_json = manifest_resp.body;
        out.local_root = download_dir;
        return out;
    };
}

}  // namespace pulp::format::reload
