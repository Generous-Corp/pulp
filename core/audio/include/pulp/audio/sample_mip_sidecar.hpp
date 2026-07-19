#pragma once

#include <pulp/audio/streaming_sample_source_file.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::audio {

enum class SampleMipSidecarStatus : std::uint8_t { Absent, Valid, Invalid };

struct SampleMipSidecar {
    static constexpr std::uint32_t kMaximumLevels = 2;

    struct Level {
        FileFrameReader reader{};
        double sample_rate = 0.0;
        std::uint32_t octave = 0;
    };

    SampleMipSidecarStatus status = SampleMipSidecarStatus::Absent;
    std::array<Level, kMaximumLevels> levels{};
    std::uint32_t level_count = 0;
};

struct SampleMipBuildOptions {
    std::uint32_t level_count = SampleMipSidecar::kMaximumLevels;
    std::uint64_t maximum_source_bytes = 512ull * 1024ull * 1024ull;
    std::uint64_t maximum_output_bytes = 512ull * 1024ull * 1024ull;
};

struct SampleMipBuildResult {
    bool ok = false;
    std::string error;
    std::string manifest_path;
    std::vector<std::string> payload_paths;
};

namespace detail {

enum class SampleMipBuildFaultForTesting {
    None,
    PayloadPublicationException,
    ManifestPolicyFinalization,
    SourceChangedAfterManifestPublication,
    PublishedManifestVerification,
    PublishedManifestVerificationException,
    PostCommitGarbageCollectionException,
};

void set_sample_mip_build_fault_for_testing(SampleMipBuildFaultForTesting fault) noexcept;

std::string sample_mip_coordination_key_for_manifest_path(
    std::string_view normalized_manifest_path, bool case_insensitive);

std::string sample_mip_coordination_key(std::string_view source_path,
                                        const runtime::FileIdentity& source_identity);

} // namespace detail

bool sample_mip_sidecar_exists(std::string_view source_path) noexcept;

std::string sample_mip_payload_path(std::string_view source_path,
                                    const std::array<std::uint8_t, 32>& source_sha256,
                                    const std::array<std::uint8_t, 32>& payload_sha256,
                                    std::uint32_t octave);

SampleMipSidecar
load_sample_mip_sidecar(std::string_view source_path, const FileFrameReader& source,
                        const std::shared_ptr<MemoryMappedAudioReader>& retained_source);

SampleMipBuildResult build_sample_mip_sidecar(std::string_view source_path,
                                              const SampleMipBuildOptions& options = {});

} // namespace pulp::audio
