#pragma once

#include <pulp/audio/sample_mip_sidecar.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::audio::sample_mip_detail {

inline constexpr std::array<std::uint8_t, 8> kMagic{'P', 'U', 'L', 'P', 'M', 'I', 'P', 0};
inline constexpr std::uint16_t kVersion = 5;
inline constexpr std::uint16_t kHeaderBytes = 116;
inline constexpr std::uint32_t kRecordBytes = 80;
inline constexpr std::uint32_t kBuilderRevision = 5;
inline constexpr std::uint64_t kMaximumManifestBytes =
    kHeaderBytes + SampleMipSidecar::kMaximumLevels * kRecordBytes;
inline constexpr std::uint64_t kMaximumWavContainerOverhead = 1024ull * 1024ull;

struct ManifestLevel {
    std::uint32_t octave = 0;
    std::uint32_t decimation = 0;
    std::uint64_t frames = 0;
    std::uint32_t rate_numerator = 0;
    std::uint32_t rate_denominator = 0;
    std::uint64_t payload_bytes = 0;
    std::array<std::uint8_t, 32> payload_sha256{};
};

using ManifestNamespace = std::array<std::uint8_t, 16>;

bool regular_file(const std::filesystem::path& path) noexcept;
std::filesystem::path normalized_source_path(std::string_view source_path);
std::filesystem::path canonical_parent_path(const std::filesystem::path& path);
ManifestNamespace default_manifest_namespace(std::string_view source_path);
std::string sample_mip_build_lock_name(std::string_view source_path,
                                       const runtime::FileIdentity& source_identity);
std::string sample_mip_publication_lock_name(std::string_view source_path,
                                             const runtime::FileIdentity& source_identity);

std::filesystem::path unique_temporary_directory(const std::filesystem::path& parent);
runtime::FileIdentity temporary_directory_identity(const std::filesystem::path& path) noexcept;
#ifdef _WIN32
bool temporary_directory_has_private_security(void* handle) noexcept;
#endif

enum class PublishResult { Published, AlreadyExists, Failed };
PublishResult publish_no_replace(const std::filesystem::path& from,
                                 const std::filesystem::path& to) noexcept;
bool replace_by_rename(const std::filesystem::path& from, const std::filesystem::path& to) noexcept;
bool sync_file_for_publication(const std::filesystem::path& path) noexcept;
bool sync_parent_directory(const std::filesystem::path& path) noexcept;

bool write_manifest(const std::filesystem::path& path, const FileFrameReader& source,
                    const runtime::FileIdentity& source_identity,
                    const ManifestNamespace& manifest_namespace,
                    const std::vector<ManifestLevel>& levels);
bool bounded_payload_file(const std::filesystem::path& path, std::uint64_t expected_bytes,
                          std::uint64_t frames, std::uint32_t channels) noexcept;
std::string sample_mip_payload_prefix(const ManifestNamespace& manifest_namespace,
                                      const std::array<std::uint8_t, 32>& source_sha256);
std::string sample_mip_namespace_prefix(const ManifestNamespace& manifest_namespace);
std::string sample_mip_payload_path_for_namespace(
    std::string_view source_path, const ManifestNamespace& manifest_namespace,
    const std::array<std::uint8_t, 32>& source_sha256,
    const std::array<std::uint8_t, 32>& payload_sha256, std::uint32_t octave);
std::optional<ManifestNamespace>
read_manifest_namespace(const std::filesystem::path& path, const FileFrameReader& source,
                        const runtime::FileIdentity& source_identity);
SampleMipSidecar load_sample_mip_sidecar_from_manifest(
    std::string_view source_path, const std::filesystem::path& manifest_path,
    const FileFrameReader& source, const runtime::FileIdentity& retained_identity);

} // namespace pulp::audio::sample_mip_detail
