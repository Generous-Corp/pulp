#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/audio/wav_decoder.hpp>
#include <pulp/playback/audio_renderer_limits.hpp>
#include <pulp/playback/stable_renderer_shell.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/model.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::playback {

class PlaybackProgram;
class ProgramCompilerTask;

enum class AudioRendererErrorCode : std::uint8_t {
    InvalidIdentity,
    DuplicateAsset,
    MissingDecodedAsset,
    InvalidAsset,
    AssetMetadataMismatch,
    UnsupportedSampleRate,
    InvalidClipRange,
    InvalidFade,
    CapacityExceeded,
    InvalidTakeComp,
};

struct AudioRendererError {
    AudioRendererErrorCode code = AudioRendererErrorCode::InvalidAsset;
    timeline::ItemId item;
    timeline::ItemId related_item;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
};

struct DecodedAudioAsset {
    timeline::ItemId id;
    std::shared_ptr<const audio::AudioFileData> audio;
};

/// Immutable, ID-sorted ownership table prepared off the audio thread.
class DecodedAudioAssetPool {
  public:
    static runtime::Result<std::shared_ptr<const DecodedAudioAssetPool>, AudioRendererError>
    create(std::vector<DecodedAudioAsset> assets, AudioRendererLimits limits = {});

    static runtime::Result<DecodedAudioAsset, AudioRendererError>
    decode_wav(timeline::ItemId id, std::span<const std::uint8_t> bytes,
               audio::WavDecodeLimits decode_limits = {});

    const DecodedAudioAsset* find(timeline::ItemId id) const noexcept;
    std::span<const DecodedAudioAsset> assets() const noexcept {
        return assets_;
    }

  private:
    explicit DecodedAudioAssetPool(std::vector<DecodedAudioAsset> assets) noexcept
        : assets_(std::move(assets)) {}
    std::vector<DecodedAudioAsset> assets_;
};

struct AudioClipRendererProgram {
    enum class SourceKind : std::uint8_t { ArrangementClip, TakeCompSegment };

    timeline::ItemId id;
    timeline::ItemId asset_id;
    std::shared_ptr<const audio::AudioFileData> audio;
    std::int64_t timeline_start = 0;
    std::uint64_t timeline_frame_count = 0;
    std::uint64_t source_start = 0;
    std::uint64_t source_frame_count = 0;
    std::uint64_t renderable_timeline_frames = 0;
    double source_frames_per_timeline_frame = 1.0;
    float gain_linear = 1.0f;
    std::uint64_t fade_in_frames = 0;
    std::uint64_t fade_out_frames = 0;
    SourceKind source_kind = SourceKind::ArrangementClip;
    std::uint32_t source_ordinal = 0;

    std::int64_t timeline_end() const noexcept;
};

class AudioTrackRendererProgram {
  public:
    timeline::ItemId id() const noexcept {
        return id_;
    }
    std::span<const AudioClipRendererProgram> clips() const noexcept {
        return clips_;
    }

  private:
    friend class ProgramCompilerTask;
    friend runtime::Result<std::shared_ptr<const AudioTrackRendererProgram>, AudioRendererError>
    link_audio_track_program(timeline::ItemId, std::vector<AudioClipRendererProgram>,
                             const AudioRendererLimits&);
    AudioTrackRendererProgram(timeline::ItemId id,
                              std::vector<AudioClipRendererProgram> clips) noexcept
        : id_(id), clips_(std::move(clips)) {}
    timeline::ItemId id_;
    std::vector<AudioClipRendererProgram> clips_;
};

runtime::Result<AudioClipRendererProgram, AudioRendererError>
compile_audio_clip_program(const timeline::Clip& clip, const timeline::Project& project,
                           const timebase::CompiledTempoMap& tempo_map,
                           const DecodedAudioAssetPool& assets, const AudioRendererLimits& limits);

/// Lowers one canonical take-comp selection to the same immutable audio-region
/// artifact used by arrangement clips. The ordinal is stable within the
/// owning lane snapshot and distinguishes repeated selections from one take.
runtime::Result<AudioClipRendererProgram, AudioRendererError> compile_take_comp_segment_program(
    const timeline::TakeLane& lane, std::size_t segment_index, const timeline::Project& project,
    const timebase::CompiledTempoMap& tempo_map, const DecodedAudioAssetPool& assets,
    const AudioRendererLimits& limits);

runtime::Result<std::shared_ptr<const AudioTrackRendererProgram>, AudioRendererError>
link_audio_track_program(timeline::ItemId track_id, std::vector<AudioClipRendererProgram> clips,
                         const AudioRendererLimits& limits);

enum class AudioRenderStatus : std::uint8_t {
    Rendered,
    Silent,
    InvalidProgram,
    InvalidTransport,
    InvalidOutput,
    CapacityExceeded,
};

/// Renders every arrangement-selected audio track in PlaybackProgram order.
/// The output is always cleared first. Mixing is deterministic float addition;
/// samples are deliberately not clipped or normalized in the engine core.
class ArrangementAudioRenderer {
  public:
    static constexpr bool carries_mutable_state = false;
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs;

    static AudioRenderStatus process(const PlaybackProgram& program,
                                     const TransportSnapshot& transport,
                                     audio::BufferView<float> output,
                                     AudioRendererLimits limits = {}) noexcept;
};

/// One stable SignalGraph-facing arrangement audio renderer. The host adapter
/// owns one instance per timeline track and supplies the callback-wide pinned
/// PlaybackProgramBlock and exact TransportSnapshot. Program changes are
/// adopted by StableRendererShell; graph custom-node state is never a program
/// publication channel.
class ArrangementAudioTrackRenderer {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    explicit ArrangementAudioTrackRenderer(timeline::ItemId track_id) noexcept : shell_(track_id) {}

    AudioRenderStatus process(const PlaybackProgramBlock& block, const TransportSnapshot& transport,
                              audio::BufferView<float> output,
                              AudioRendererLimits limits = {}) noexcept;

    RendererProgramKey active_key() const noexcept {
        return shell_.active_key();
    }
    RendererCarryState state_snapshot() const noexcept {
        return shell_.state_snapshot();
    }
    void reset() noexcept { shell_.reset(); }

  private:
    StableRendererShell shell_;
};

} // namespace pulp::playback
