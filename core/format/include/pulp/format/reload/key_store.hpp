#pragma once

// Signing-key material + a file-backed store for the swap-pack signing flow. The
// key is an Ed25519 keypair; it is serialized as a small text blob (base64 public +
// private) so it can live in a file OR a keychain/GitHub secret (same bytes, same
// parse). load_or_generate_file() reuses an existing key and NEVER silently makes a
// second one — a plugin's key is its identity. The caller prints the loud
// provenance banner when `generated` comes back true.

#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::format::reload {

struct KeyMaterial {
    std::vector<std::uint8_t> public_key;
    std::vector<std::uint8_t> private_key;
    bool valid() const { return !public_key.empty() && !private_key.empty(); }
};

inline constexpr std::string_view kKeyBlobMagic = "PULP-RELOAD-KEY-v1";

/// Serialize a keypair to a text blob: magic line, base64 public, base64 private.
inline std::string serialize_key_blob(const KeyMaterial& k) {
    return std::string(kKeyBlobMagic) + "\n" +
           runtime::base64_encode(k.public_key.data(), k.public_key.size()) + "\n" +
           runtime::base64_encode(k.private_key.data(), k.private_key.size()) + "\n";
}

/// Parse a key blob. Returns nullopt on a bad magic line or non-base64 fields
/// (fail closed — a malformed key never silently yields empty material).
inline std::optional<KeyMaterial> parse_key_blob(std::string_view blob) {
    auto next_line = [&](std::size_t& pos) -> std::string_view {
        const std::size_t nl = blob.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? blob.size() : nl;
        std::string_view line = blob.substr(pos, end - pos);
        pos = nl == std::string_view::npos ? blob.size() : nl + 1;
        // trim a trailing CR (tolerate CRLF).
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return line;
    };
    std::size_t pos = 0;
    if (next_line(pos) != kKeyBlobMagic) return std::nullopt;
    const std::string_view pub_b64 = next_line(pos);
    const std::string_view priv_b64 = next_line(pos);
    auto pub = runtime::base64_decode(pub_b64);
    auto priv = runtime::base64_decode(priv_b64);
    if (!pub || !priv || pub->empty() || priv->empty()) return std::nullopt;
    return KeyMaterial{std::move(*pub), std::move(*priv)};
}

/// Load the key at @p path, or generate a fresh keypair, write it there (0600), and
/// return it with @p generated=true so the caller can scream. Never regenerates an
/// existing key. Returns nullopt only on generation failure or an unreadable/corrupt
/// existing file (fail closed rather than silently minting a second identity).
inline std::optional<KeyMaterial> load_or_generate_key_file(const std::filesystem::path& path,
                                                            bool& generated) {
    generated = false;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        std::string blob((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        return parse_key_blob(blob);  // nullopt on corruption → caller errors, no regen
    }
    auto kp = runtime::ed25519_keypair_generate();
    if (!kp) return std::nullopt;
    KeyMaterial km{kp->public_key, kp->private_key};
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return std::nullopt;
        out << serialize_key_blob(km);
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    generated = true;
    return km;
}

}  // namespace pulp::format::reload
