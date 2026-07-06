#pragma once

/// @file swap_pack.hpp
/// Sealed swap-pack manifest + per-file integrity.
///
/// A swap-pack is downloadable content that hot-swaps a shipped plugin's UI/DSP:
/// a manifest plus files (scripted-UI JS + theme + assets, `.pulpgraph` DSP
/// graphs, node packs, reserved `wasm-dsp`). This slice defines the manifest
/// schema and the FIRST verification layer — per-file SHA-256 — so a tampered or
/// truncated pack fails closed at the hash layer before anything is installed or
/// swapped. It reuses `runtime::sha256_hex` and mirrors the proven
/// `NodePackManifest` scheme (per-file hash → typed reject codes).
///
/// Deferred to later slices (see planning/2026-07-02-phase3-consumer-substrate-
/// scoping.md): the Ed25519 pack SIGNATURE (3.1b), registry install + driving the
/// live swap (3.1c). This header adds NO signing and does not install anything.

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pulp::format::reload {

/// Payload kind for one file in a swap pack. `WasmDsp` is reserved (the Phase-4
/// lane); `Unknown` is any unrecognized kind (accepted structurally but the
/// installer/registry decides whether to honor it).
enum class SwapPackKind { UiScript, DspGraph, NodePack, WasmDsp, Unknown };

inline SwapPackKind swap_pack_kind_from_string(std::string_view s) {
    if (s == "ui-script") return SwapPackKind::UiScript;
    if (s == "dsp-graph") return SwapPackKind::DspGraph;
    if (s == "node-pack") return SwapPackKind::NodePack;
    if (s == "wasm-dsp")  return SwapPackKind::WasmDsp;
    return SwapPackKind::Unknown;
}

/// Stable canonical spelling of a kind, for the signed message (never localized
/// / reformatted). Must round-trip with swap_pack_kind_from_string.
inline const char* swap_pack_kind_to_string(SwapPackKind k) {
    switch (k) {
        case SwapPackKind::UiScript: return "ui-script";
        case SwapPackKind::DspGraph: return "dsp-graph";
        case SwapPackKind::NodePack: return "node-pack";
        case SwapPackKind::WasmDsp:  return "wasm-dsp";
        case SwapPackKind::Unknown:  break;
    }
    return "unknown";
}

struct SwapPackFile {
    std::string path;        ///< path relative to the pack root
    std::string sha256_hex;  ///< expected lowercase hex SHA-256 of the file bytes
    SwapPackKind kind = SwapPackKind::Unknown;
};

struct SwapPackManifest {
    std::string id;             ///< pack identity
    std::string plugin_id;      ///< the plugin this pack targets
    int format_version = 1;
    std::vector<SwapPackFile> files;
    // Policy fields. These are covered by the signature (see swap_pack_signed_message)
    // so a CDN or installer cannot swap the declared capabilities, version, or kind
    // while keeping a valid signature — the manifest is the single sanctioned place
    // these are trusted from.
    std::uint64_t pack_version = 0;               ///< monotonic; a loader rejects a downgrade
    SwapPackKind pack_type = SwapPackKind::Unknown; ///< overall pack kind (gates delivery lanes)
    std::vector<std::string> declared_capabilities; ///< capability names the UI is granted
    std::string update_channel;                   ///< delivery channel (also namespaces revocation)
    int min_host_version = 0;                     ///< minimum host ABI/app version that may load it
    std::vector<std::uint8_t> signer_public_key;  ///< 32 bytes (Ed25519), empty if unsigned
    std::vector<std::uint8_t> signature;          ///< 64 bytes (Ed25519 detached), empty if unsigned
};

enum class SwapPackVerify {
    Ok = 0,
    ManifestParseError,   ///< manifest JSON malformed or missing required fields
    MissingFile,          ///< a declared file is absent under the pack root
    ReadError,            ///< a declared file could not be read
    HashMismatch,         ///< a file's SHA-256 != the manifest's declared hash
    UntrustedSigner,      ///< the manifest's signer key is not the trusted key
    BadSignature,         ///< Ed25519 verification failed (or malformed sig/key sizes)
};

struct SwapPackVerifyResult {
    SwapPackVerify status = SwapPackVerify::Ok;
    std::string detail;   ///< which file / what went wrong (diagnostics)
    bool ok() const { return status == SwapPackVerify::Ok; }
};

/// Decode a lowercase/uppercase hex string to bytes. Returns nullopt on an odd
/// length or a non-hex digit (fail-closed — a malformed key/sig never verifies).
inline std::optional<std::vector<std::uint8_t>> swap_pack_hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

/// Parse a swap-pack manifest from JSON. Returns nullopt on any structural
/// problem (missing/mistyped required field), writing a reason to @p error.
/// Required: string `id`, string `plugin_id`, array `files` of
/// `{string path, string sha256, string kind}`. `format_version` defaults to 1.
inline std::optional<SwapPackManifest> parse_swap_pack_manifest(std::string_view json,
                                                                std::string& error) {
    SwapPackManifest m;
    try {
        auto v = choc::json::parse(std::string(json));
        if (!v.isObject()) { error = "manifest is not a JSON object"; return std::nullopt; }
        if (!v.hasObjectMember("id") || !v["id"].isString()) {
            error = "missing/invalid string 'id'"; return std::nullopt;
        }
        if (!v.hasObjectMember("plugin_id") || !v["plugin_id"].isString()) {
            error = "missing/invalid string 'plugin_id'"; return std::nullopt;
        }
        m.id = v["id"].getString();
        m.plugin_id = v["plugin_id"].getString();
        if (v.hasObjectMember("format_version") && v["format_version"].isInt())
            m.format_version = static_cast<int>(v["format_version"].getInt64());
        if (!v.hasObjectMember("files") || !v["files"].isArray()) {
            error = "missing/invalid array 'files'"; return std::nullopt;
        }
        const auto& files = v["files"];
        for (uint32_t i = 0; i < files.size(); ++i) {
            const auto& f = files[i];
            if (!f.isObject() || !f.hasObjectMember("path") || !f["path"].isString() ||
                !f.hasObjectMember("sha256") || !f["sha256"].isString()) {
                error = "files[" + std::to_string(i) + "] missing string 'path'/'sha256'";
                return std::nullopt;
            }
            SwapPackFile pf;
            pf.path = f["path"].getString();
            pf.sha256_hex = f["sha256"].getString();
            pf.kind = f.hasObjectMember("kind") && f["kind"].isString()
                          ? swap_pack_kind_from_string(f["kind"].getString())
                          : SwapPackKind::Unknown;
            m.files.push_back(std::move(pf));
        }
        // Policy fields. All optional with safe defaults so an older manifest still
        // parses; whatever is present is bound into the signed message below.
        if (v.hasObjectMember("pack_version") && v["pack_version"].isInt())
            m.pack_version = static_cast<std::uint64_t>(v["pack_version"].getInt64());
        if (v.hasObjectMember("pack_type") && v["pack_type"].isString())
            m.pack_type = swap_pack_kind_from_string(v["pack_type"].getString());
        if (v.hasObjectMember("update_channel") && v["update_channel"].isString())
            m.update_channel = v["update_channel"].getString();
        if (v.hasObjectMember("min_host_version") && v["min_host_version"].isInt())
            m.min_host_version = static_cast<int>(v["min_host_version"].getInt64());
        if (v.hasObjectMember("capabilities") && v["capabilities"].isArray()) {
            const auto& caps = v["capabilities"];
            for (uint32_t i = 0; i < caps.size(); ++i)
                if (caps[i].isString()) m.declared_capabilities.emplace_back(caps[i].getString());
        }
        // Optional signature fields (hex). Absent → unsigned manifest (empty
        // vectors); a malformed hex string is left empty so verification fails
        // closed rather than parsing to garbage bytes.
        if (v.hasObjectMember("signer") && v["signer"].isString()) {
            if (auto k = swap_pack_hex_decode(v["signer"].getString())) m.signer_public_key = std::move(*k);
        }
        if (v.hasObjectMember("signature") && v["signature"].isString()) {
            if (auto s = swap_pack_hex_decode(v["signature"].getString())) m.signature = std::move(*s);
        }
    } catch (const std::exception& e) {
        error = std::string("manifest JSON parse failed: ") + e.what();
        return std::nullopt;
    }
    return m;
}

/// Verify every declared file exists under @p root and its bytes hash to the
/// declared SHA-256. Fail-closed: the first problem wins. Control thread (reads
/// files + hashes); does not install or mutate anything.
inline SwapPackVerifyResult verify_swap_pack_integrity(const std::filesystem::path& root,
                                                       const SwapPackManifest& manifest) {
    auto to_lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (const auto& f : manifest.files) {
        const auto full = root / f.path;
        std::error_code ec;
        if (!std::filesystem::exists(full, ec) || ec) {
            return {SwapPackVerify::MissingFile, f.path};
        }
        std::ifstream in(full, std::ios::binary);
        if (!in) return {SwapPackVerify::ReadError, f.path};
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        if (in.bad()) return {SwapPackVerify::ReadError, f.path};
        const std::string actual = runtime::sha256_hex(bytes.data(), bytes.size());
        if (to_lower(actual) != to_lower(f.sha256_hex)) {
            return {SwapPackVerify::HashMismatch, f.path};
        }
    }
    return {SwapPackVerify::Ok, ""};
}

/// Deterministic canonical payload the pack signature covers (item 3.1b). Binds
/// pack identity, target plugin, format version, and every file's path + hash +
/// kind — so a tampered file hash OR a re-pointed/re-kinded file breaks the
/// signature. Independent of JSON formatting (re-serialization can't break it).
///
/// Every variable-length field is LENGTH-PREFIXED (u32 LE) and the file count is
/// bound, so the encoding is unambiguous: unlike a newline-delimited join, no
/// field value (even one containing newlines or the "files" label) can be
/// reinterpreted to collide with a different manifest. The signature field itself
/// is NOT bound. (Hardening over the node_pack '\n'-join scheme — review 3.1b.)
inline std::vector<std::uint8_t> swap_pack_signed_message(const SwapPackManifest& m) {
    std::vector<std::uint8_t> out;
    auto put_u32 = [&out](std::uint32_t n) {
        out.push_back(static_cast<std::uint8_t>(n & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFF));
    };
    auto put_field = [&](std::string_view s) {
        put_u32(static_cast<std::uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    };
    auto put_u64 = [&out](std::uint64_t n) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((n >> (8 * i)) & 0xFF));
    };
    // v2 binds the policy fields (version, kind, capabilities, channel, min-host)
    // into the signature so a CDN/installer cannot alter them while keeping a valid
    // signature. The tag bump also means a v1 signature can never verify as v2.
    put_field("pulp-swap-pack-v2");
    put_field(m.id);
    put_field(m.plugin_id);
    put_u32(static_cast<std::uint32_t>(m.format_version));
    put_u64(m.pack_version);
    put_field(swap_pack_kind_to_string(m.pack_type));
    put_field(m.update_channel);
    put_u32(static_cast<std::uint32_t>(m.min_host_version));
    put_u32(static_cast<std::uint32_t>(m.declared_capabilities.size()));  // bind the count
    for (const auto& c : m.declared_capabilities) put_field(c);
    put_u32(static_cast<std::uint32_t>(m.files.size()));   // bind the file count
    for (const auto& f : m.files) {
        put_field(f.path);
        put_field(f.sha256_hex);
        put_field(swap_pack_kind_to_string(f.kind));
    }
    return out;
}

/// Verify the pack's Ed25519 signature (item 3.1b). Fails closed on: a signer key
/// that is not @p trusted_public_key (UntrustedSigner), or a malformed/inauthentic
/// signature (BadSignature). Does NOT touch files — pair with
/// verify_swap_pack_integrity for the hash layer (or use verify_swap_pack).
inline SwapPackVerifyResult verify_swap_pack_signature(
    const SwapPackManifest& manifest,
    const std::vector<std::uint8_t>& trusted_public_key) {
    // Trust root: the signer key must be exactly the trusted key. (Key
    // distribution / a multi-key trust store is an owner-steer decision — see the
    // Phase-3 scoping note; this takes a single trusted key by value.)
    if (manifest.signer_public_key.size() != runtime::ed25519_public_key_size ||
        trusted_public_key.size() != runtime::ed25519_public_key_size ||
        manifest.signer_public_key != trusted_public_key) {
        return {SwapPackVerify::UntrustedSigner, "signer key is not the trusted key"};
    }
    if (manifest.signature.size() != runtime::ed25519_signature_size) {
        return {SwapPackVerify::BadSignature, "signature size invalid"};
    }
    const auto message = swap_pack_signed_message(manifest);
    if (!runtime::ed25519_verify(manifest.signer_public_key.data(),
                                 manifest.signer_public_key.size(),
                                 manifest.signature.data(), manifest.signature.size(),
                                 message.data(), message.size())) {
        return {SwapPackVerify::BadSignature, "Ed25519 verification failed"};
    }
    return {SwapPackVerify::Ok, ""};
}

/// Full fail-closed verify: signature FIRST (binds the file hashes without
/// reading the files), THEN per-file integrity (the files match the signed
/// hashes). Order mirrors node_pack: authenticate before touching content.
///
/// This is the ONLY sanctioned "is this pack trustworthy?" entry point. The
/// install path (3.1c) must call this — never verify_swap_pack_integrity alone
/// (that proves the files match the manifest, NOT that the manifest is signed).
inline SwapPackVerifyResult verify_swap_pack(
    const std::filesystem::path& root, const SwapPackManifest& manifest,
    const std::vector<std::uint8_t>& trusted_public_key) {
    auto sig = verify_swap_pack_signature(manifest, trusted_public_key);
    if (!sig.ok()) return sig;
    return verify_swap_pack_integrity(root, manifest);
}

}  // namespace pulp::format::reload
