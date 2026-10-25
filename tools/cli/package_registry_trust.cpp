#include "package_registry_trust.hpp"

#include <pulp/runtime/crypto.hpp>

#include <cctype>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulp::cli::pkg {

namespace {

std::optional<std::vector<std::uint8_t>> hex_decode(std::string_view hex) {
    if (hex.size() % 2 != 0)
        return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        c = static_cast<unsigned char>(std::tolower(c));
        if (c >= 'a' && c <= 'f')
            return 10 + (c - 'a');
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(static_cast<unsigned char>(hex[i]));
        const int lo = nibble(static_cast<unsigned char>(hex[i + 1]));
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> non_empty_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto end = text.find('\n', pos);
        const auto line = trim(
            text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        if (!line.empty())
            lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }
    return lines;
}

RegistrySignatureCheck fail(std::string code, std::string error) {
    RegistrySignatureCheck out;
    out.code = std::move(code);
    out.error = std::move(error);
    return out;
}

} // namespace

std::string registry_signed_message(std::string_view registry_bytes) {
    return std::string(kRegistrySignatureSchema) + "\n" +
           pulp::runtime::sha256_hex(registry_bytes) + "\n";
}

RegistrySignatureCheck verify_registry_signature(std::string_view registry_bytes,
                                                 std::string_view signature_text,
                                                 std::string_view pinned_public_key_hex) {
    const auto public_key = hex_decode(trim(pinned_public_key_hex));
    if (!public_key || public_key->size() != pulp::runtime::ed25519_public_key_size)
        return fail(
            "no-trust-root",
            "this build pins no registry signing key, so the remote registry cannot be verified");
    if (trim(signature_text).empty())
        return fail("missing-signature", "the remote registry has no registry.json.sig beside it");

    const auto lines = non_empty_lines(signature_text);
    if (lines.size() != 2 || lines[0] != kRegistrySignatureSchema)
        return fail("malformed-signature", "registry.json.sig must be two lines: `" +
                                               std::string(kRegistrySignatureSchema) +
                                               "` and the hex Ed25519 signature");
    const auto signature = hex_decode(lines[1]);
    if (!signature || signature->size() != pulp::runtime::ed25519_signature_size)
        return fail(
            "malformed-signature",
            "registry.json.sig signature must be a 64-byte Ed25519 signature encoded as hex");

    const std::string message = registry_signed_message(registry_bytes);
    if (!pulp::runtime::ed25519_verify(
            public_key->data(), public_key->size(), signature->data(), signature->size(),
            reinterpret_cast<const std::uint8_t*>(message.data()), message.size()))
        return fail("signature-mismatch",
                    "registry.json.sig does not verify the registry bytes with the pinned key");

    RegistrySignatureCheck out;
    out.ok = true;
    out.code = "verified";
    return out;
}

} // namespace pulp::cli::pkg
