#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/finite_time_stretch.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <compare>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::playback {

struct DecodedAudioAsset;
enum class OfflineStretchErrorCode : std::uint8_t;

namespace detail {
bool offline_stretch_frame_distance(std::int64_t start, std::int64_t end,
                                    std::uint64_t& distance) noexcept;
OfflineStretchErrorCode
offline_stretch_error_code(audio::FiniteTimeStretchFailure failure) noexcept;
}

inline constexpr std::uint32_t kOfflineStretchAlgorithmVersion = 1;

struct OfflineStretchAlgorithmConfig {
    std::uint32_t version = kOfflineStretchAlgorithmVersion;
    float max_time_ratio = 16.0f;
    constexpr auto operator<=>(const OfflineStretchAlgorithmConfig&) const = default;
};

struct OfflineStretchArtifactKey {
    timeline::ContentHash source_content_hash;
    timeline::ContentHash decoded_content_hash;
    std::uint64_t source_start = 0;
    std::uint64_t source_frame_count = 0;
    timebase::RationalRate source_sample_rate;
    timebase::RationalRate timeline_sample_rate;
    timebase::TickPosition musical_tick_start;
    timebase::TickPosition musical_tick_end;
    std::uint64_t timeline_input_frame_count = 0;
    std::uint64_t target_frame_count = 0;
    std::uint32_t channel_count = 0;
    OfflineStretchAlgorithmConfig algorithm;
    std::vector<timebase::TempoPoint> tempo_points;

    bool operator==(const OfflineStretchArtifactKey&) const = default;
};

struct OfflineStretchArtifact {
    OfflineStretchArtifactKey key;
    std::shared_ptr<const audio::AudioFileData> audio;
};

struct OfflineStretchProvenance {
    timeline::ItemId clip_id;
    timeline::ItemId project_id;
    std::uint64_t document_revision = 0;
    std::uint64_t program_generation = 0;
    bool cache_hit = false;

    bool matches(timeline::ItemId clip, timeline::ItemId project, std::uint64_t revision,
                 std::uint64_t generation) const noexcept {
        return clip_id == clip && project_id == project && document_revision == revision &&
               program_generation == generation;
    }
};

enum class OfflineStretchErrorCode : std::uint8_t {
    None,
    InvalidClip,
    MissingSource,
    AssetMetadataMismatch,
    UnsupportedSampleRate,
    InvalidAlgorithmConfig,
    InvalidTempoSchedule,
    CapacityExceeded,
    SampleRateConverterPrepareFailed,
    ProcessorPrepareFailed,
    InvalidRatio,
    OutputTooShort,
    OutputTooLong,
    ProcessorProtocolError,
};

struct OfflineStretchError {
    OfflineStretchErrorCode code = OfflineStretchErrorCode::None;
    timeline::ItemId item;
    timeline::ItemId related_item;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
};

struct OfflineStretchLimits {
    std::uint64_t max_input_frames = 100'000'000u;
    std::uint64_t max_output_frames = 100'000'000u;
    std::uint64_t max_input_bytes = 1024u * 1024u * 1024u;
    std::uint64_t max_sample_rate_converter_bytes = 256u * 1024u * 1024u;
    std::uint64_t max_scratch_allocation_bytes = 256u * 1024u * 1024u;
    std::uint64_t max_output_bytes = 1024u * 1024u * 1024u;
    std::uint64_t max_artifact_bytes = 512u * 1024u * 1024u;
    std::uint64_t max_cache_bytes = 2u << 30u;
    std::uint32_t max_cached_artifacts = 256;
    constexpr auto operator<=>(const OfflineStretchLimits&) const = default;
};

/// Control-thread-only bounded semantic artifact cache.
class OfflineStretchArtifactCache {
  public:
    std::shared_ptr<const OfflineStretchArtifact>
    find(const OfflineStretchArtifactKey& key) const noexcept;
    bool insert(std::shared_ptr<const OfflineStretchArtifact> artifact,
                OfflineStretchLimits limits) noexcept;
    void clear() noexcept;
    void constrain(OfflineStretchLimits limits) noexcept;
    std::uint64_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    std::size_t size() const noexcept {
        return artifacts_.size();
    }

  private:
    std::vector<std::shared_ptr<const OfflineStretchArtifact>> artifacts_;
    std::uint64_t retained_bytes_ = 0;
};

enum class OfflineStretchCompileStatus : std::uint8_t { Progress, Complete, Failed };

/// Control-thread-only resumable source-SRC-stretch-seal pipeline.
class OfflineStretchCompileJob {
  public:
    OfflineStretchCompileJob();
    ~OfflineStretchCompileJob();
    OfflineStretchCompileJob(OfflineStretchCompileJob&&) noexcept;
    OfflineStretchCompileJob& operator=(OfflineStretchCompileJob&&) noexcept;
    OfflineStretchCompileJob(const OfflineStretchCompileJob&) = delete;
    OfflineStretchCompileJob& operator=(const OfflineStretchCompileJob&) = delete;

    OfflineStretchCompileStatus begin(const timeline::Clip& clip, const timeline::Project& project,
                                      const timebase::CompiledTempoMap& tempo_map,
                                      const DecodedAudioAsset& decoded,
                                      OfflineStretchLimits limits = {},
                                      OfflineStretchAlgorithmConfig algorithm = {},
                                      std::uint32_t work_block_frames = 256,
                                      OfflineStretchArtifactCache* cache = nullptr);
    OfflineStretchCompileStatus step() noexcept;

    OfflineStretchCompileStatus status() const noexcept;
    OfflineStretchError error() const noexcept;
    bool cache_hit() const noexcept;
    const OfflineStretchArtifactKey* key() const noexcept;
    std::shared_ptr<const OfflineStretchArtifact> take() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::playback
