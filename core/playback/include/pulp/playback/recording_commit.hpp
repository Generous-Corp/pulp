#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/playback/capture_engine.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timeline/command.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::playback {

struct RecordingTakeCommitRequest {
    timeline::ItemId sequence_id;
    timeline::ItemId track_id;
    timeline::ItemId take_lane_id;
    timeline::ItemId asset_id;
    timeline::ItemId take_id;
    timebase::SamplePosition placement_start;
    timebase::RationalRate sample_rate;
    std::string asset_name;
    bool create_take_lane = false;
    std::string take_lane_name;
};

struct SealedRecordingTake {
    timeline::MediaAsset asset;
    timeline::Take take;
    std::vector<std::uint8_t> wav_bytes;
    std::vector<timeline::Command> commands;
};

enum class RecordingCommitError : std::uint8_t {
    InvalidRequest,
    UnsupportedWaveShape,
    WaveSizeExceeded,
    HashFailure,
    InvalidTake,
    InvalidTakeLane,
    RetrospectiveUnavailable,
};

runtime::Result<SealedRecordingTake, RecordingCommitError>
seal_recording_take(const audio::BufferView<const float>& audio,
                    RecordingTakeCommitRequest request);

runtime::Result<SealedRecordingTake, RecordingCommitError>
seal_retrospective_take(const audio::RollingAudioCaptureBuffer& capture,
                        const audio::RollingAudioCaptureHold& hold,
                        RecordingTakeCommitRequest request);

} // namespace pulp::playback
