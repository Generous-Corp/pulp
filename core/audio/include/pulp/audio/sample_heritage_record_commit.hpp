#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_schema.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::audio {

inline constexpr std::uint32_t kSampleHeritageCommittedAssetSchemaVersion = 1;

enum class SampleHeritageRecordCommitStatus : std::uint8_t {
    Ok,
    InvalidProfile,
    InvalidSource,
    InvalidProvenance,
    InvalidRecordChain,
    SizeOverflow,
    AllocationFailed,
    InvalidMetadata,
    NonCanonicalMetadata,
    ProfileMismatch,
    AudioHashMismatch,
};

enum class SampleHeritageAutoCycleStatus : std::uint8_t {
    Ok,
    InvalidSource,
    InvalidRange,
    InsufficientSignal,
    NoReliableCycle,
};

struct SampleHeritageAutoCycleOptions {
    std::uint32_t minimum_cycle_samples = 16;
    std::uint32_t maximum_cycle_samples = 4096;
    std::uint64_t analysis_start_frame = 0;
    std::uint64_t analysis_end_frame = 0;
    double minimum_correlation = 0.6;
};

struct SampleHeritageAutoCycleResult {
    SampleHeritageAutoCycleStatus status =
        SampleHeritageAutoCycleStatus::InvalidSource;
    std::uint32_t cycle_samples = 0;
    double correlation = 0.0;

    bool valid() const noexcept {
        return status == SampleHeritageAutoCycleStatus::Ok;
    }
};

struct SampleHeritageRecordProvenance {
    std::string source_id;
    std::string capture_method;
    std::string evidence_id;
};

struct SampleHeritageCommittedAssetMetadata {
    std::uint32_t schema_version = kSampleHeritageCommittedAssetSchemaVersion;
    std::uint32_t profile_schema_version = 0;
    std::string profile_id;
    std::string profile_digest_sha256;
    double source_sample_rate = 0.0;
    std::uint64_t source_frames = 0;
    std::uint32_t source_channels = 0;
    std::string source_audio_sha256;
    double committed_sample_rate = 0.0;
    std::uint64_t committed_frames = 0;
    std::uint32_t committed_channels = 0;
    std::string committed_audio_sha256;
    SampleHeritageRecordProvenance provenance;
};

/// Immutable result of the allocating, off-audio-thread record-commit path.
class SampleHeritageCommittedAsset {
public:
    SampleHeritageCommittedAsset(Buffer<float> audio,
                                 SampleHeritageCommittedAssetMetadata metadata,
                                 std::string canonical_metadata_json)
        : audio_(std::move(audio)),
          metadata_(std::move(metadata)),
          canonical_metadata_json_(std::move(canonical_metadata_json)) {}

    const Buffer<float>& audio() const noexcept { return audio_; }
    const SampleHeritageCommittedAssetMetadata& metadata() const noexcept {
        return metadata_;
    }
    std::string_view canonical_metadata_json() const noexcept {
        return canonical_metadata_json_;
    }

private:
    Buffer<float> audio_;
    SampleHeritageCommittedAssetMetadata metadata_;
    std::string canonical_metadata_json_;
};

struct SampleHeritageRecordCommitResult {
    SampleHeritageRecordCommitStatus status =
        SampleHeritageRecordCommitStatus::InvalidProfile;
    std::string detail;
    std::optional<SampleHeritageCommittedAsset> asset;

    bool valid() const noexcept {
        return status == SampleHeritageRecordCommitStatus::Ok && asset.has_value();
    }
};

/// Suggests a repeatable cycle length from periodic similarity in an audio
/// region. The returned cycle is an explicit neutral estimate, not a model of
/// any hardware auto-cycle control. This offline helper is not suitable for the
/// audio thread.
SampleHeritageAutoCycleResult estimate_sample_heritage_auto_cycle(
    BufferView<const float> source,
    const SampleHeritageAutoCycleOptions& options = {});

/// Applies the profile's record_commit recipe and returns machine-rate PCM plus
/// a canonical, content-addressed metadata envelope. Allocates throughout and
/// must never run on the audio thread. This self-contained transaction accepts
/// RestartFromProfileSeed converters; ContinueSerializedState requires prior
/// converter state and is rejected by this API.
SampleHeritageRecordCommitResult
commit_sample_heritage_recording(const SampleHeritageProfile& profile,
    BufferView<const float> source,
    double source_sample_rate,
    const SampleHeritageRecordProvenance& provenance);

/// Verifies canonical metadata, profile identity, dimensions, and committed
/// PCM hash before publishing an owned asset. Allocates and is off-thread only.
SampleHeritageRecordCommitResult reload_sample_heritage_committed_asset(
    const SampleHeritageProfile& profile,
    BufferView<const float> committed_audio,
    double committed_sample_rate,
    std::string_view canonical_metadata_json);

}  // namespace pulp::audio
