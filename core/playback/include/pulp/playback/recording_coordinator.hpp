#pragma once

#include <pulp/audio/audio_io_timing.hpp>
#include <pulp/playback/capture_engine.hpp>
#include <pulp/playback/recording_commit.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timeline/document_session.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::playback {

enum class RecordingMonitorMode : std::uint8_t { Off, Direct, Software, Auto };
enum class RecordingMonitorPath : std::uint8_t { Off, Direct, Software };

struct RecordingSource {
    std::string id;
    std::uint32_t input_channel = 0;
    std::uint32_t output_channel = 0;
    std::uint32_t channel_count = 0;
};

struct RecordingTrackConfig {
    timeline::ItemId track_id;
    timeline::ItemId take_lane_id;
    RecordingSource source;
    bool armed = false;
    bool capture_midi = false;
    RecordingMonitorMode monitoring = RecordingMonitorMode::Auto;
    bool direct_monitoring_available = false;
};

struct RecordingCoordinatorConfig {
    timebase::RationalRate sample_rate;
    std::uint32_t maximum_block_size = 0;
    std::uint64_t maximum_take_frames = 0;
    std::uint32_t take_slots_per_track = 0;
    std::uint32_t midi_events_per_take = 0;
    std::uint64_t maximum_preallocated_bytes = CaptureEngineConfig::kDefaultMaximumPreallocatedBytes;
    audio::LatencySnapshot latency;
    audio::AudioIoTiming current_audio_timing;
    std::vector<RecordingTrackConfig> tracks;
};

/// Package-neutral durability boundary used before a Timeline transaction is
/// published. A project-package adapter implements this with
/// PackageWriter::stage_blob(BlobStore::Media, ...).
class RecordingMediaStager {
  public:
    virtual ~RecordingMediaStager() = default;
    virtual bool stage_media(const timeline::ContentHash& hash,
                             std::span<const std::uint8_t> bytes) noexcept = 0;
};

enum class RecordingCoordinatorError : std::uint8_t {
    NotPrepared,
    StaleLatency,
    InvalidTake,
    CaptureCopyFailed,
    SealFailed,
    MediaStageFailed,
    DocumentCommitFailed,
};

struct CommittedRecordingTake {
    RecordingSource source;
    RecordingMonitorPath monitoring = RecordingMonitorPath::Off;
    timeline::MediaAsset asset;
    timeline::Take take;
    timeline::CommitResult commit;
};

/// Control-thread coordinator for capture completion, calibrated placement,
/// media durability, and one atomic Timeline publication.
class RecordingCoordinator {
  public:
    bool prepare(const RecordingCoordinatorConfig& config);
    void release() noexcept;

    bool enqueue_command(const CaptureCommand& command) noexcept;
    bool pop_event(CaptureEvent& event) noexcept;
    CaptureProcessResult process(const audio::BufferView<const float>& input,
                                 audio::BufferView<float>& monitor_output,
                                 const midi::MidiBuffer& midi_input,
                                 const TransportSnapshot& transport) noexcept;

    RecordingMonitorPath monitoring_path(std::size_t track_index) const noexcept;

    runtime::Result<CommittedRecordingTake, RecordingCoordinatorError>
    commit_take(const CaptureEvent& completed, RecordingTakeCommitRequest request,
                const audio::AudioIoTiming& current_audio_timing,
                RecordingMediaStager& stager, timeline::DocumentSession& session,
                timeline::WriterToken& writer);

  private:
    CaptureEngine capture_;
    audio::Buffer<float> monitor_scratch_;
    RecordingCoordinatorConfig config_;
    std::vector<RecordingMonitorPath> monitoring_paths_;
    std::vector<CaptureEvent> completed_events_;
    std::vector<std::uint32_t> committed_generations_;
    std::vector<std::uint32_t> pending_release_generations_;
    bool prepared_ = false;

    void flush_pending_releases() noexcept;
};

} // namespace pulp::playback
