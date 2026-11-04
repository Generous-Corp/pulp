// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <limits>
#include <pulp/host/plugin_identity.hpp>
#include <pulp/runtime/crypto.hpp>

namespace pulp::host::detail {
inline PluginIdentityEvidence ClapIdentityAccess::observed(PluginTypeIdentity type,
                                                           PluginArtifactProvenance artifact,
                                                           bool loaded) {
    return {std::move(type), PluginIdentityOrigin::clap_descriptor,
            loaded ? "pulp.clap.exact-loader.v1" : "pulp.clap.descriptor-scanner.v1",
            std::move(artifact),
            loaded ? PluginIdentityVerification::loaded : PluginIdentityVerification::declared};
}

inline std::optional<PluginArtifactProvenance> clap_artifact_provenance(const std::string& binary) {
    std::error_code error;
    auto size = std::filesystem::file_size(binary, error);
    if (error)
        return std::nullopt;
    // Bound work on an untrusted artifact; never use path/mtime as identity.
    constexpr std::uint64_t max_bytes = 1ULL << 30;
    auto hash = runtime::sha256_file_hex(binary, max_bytes);
    if (!hash || std::filesystem::file_size(binary, error) != size || error)
        return std::nullopt;
    return PluginArtifactProvenance{std::move(*hash), size, std::nullopt};
}

inline std::variant<std::string, PluginIdentityError> clap_descriptor_id(const char* id) {
    if (!id || !*id)
        return PluginIdentityError::native_identity_missing;
    // The ABI supplies NUL-terminated strings. Bound traversal for malformed IDs.
    constexpr std::size_t max_bytes = 4096;
    std::size_t length = 0;
    while (length <= max_bytes && id[length])
        ++length;
    if (length > max_bytes)
        return PluginIdentityError::invalid_native_identity;
    std::string owned(id, length);
    if (auto error = validate_plugin_type_identity({"clap", owned}))
        return *error;
    return owned;
}
} // namespace pulp::host::detail
