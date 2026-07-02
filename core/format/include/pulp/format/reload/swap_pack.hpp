#pragma once

/// @file swap_pack.hpp
/// Sealed swap-pack manifest + per-file integrity (live-swap plan item 3.1a).
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
    // Ed25519 signer_public_key + signature are added in 3.1b.
};

enum class SwapPackVerify {
    Ok = 0,
    ManifestParseError,   ///< manifest JSON malformed or missing required fields
    MissingFile,          ///< a declared file is absent under the pack root
    ReadError,            ///< a declared file could not be read
    HashMismatch,         ///< a file's SHA-256 != the manifest's declared hash
};

struct SwapPackVerifyResult {
    SwapPackVerify status = SwapPackVerify::Ok;
    std::string detail;   ///< which file / what went wrong (diagnostics)
    bool ok() const { return status == SwapPackVerify::Ok; }
};

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

}  // namespace pulp::format::reload
