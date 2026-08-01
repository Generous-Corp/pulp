#pragma once

#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/compile_executor.hpp>
#include <pulp/playback/dirty_track_resolver.hpp>
#include <pulp/playback/offline_stretch_artifact.hpp>
#include <pulp/playback/program.hpp>
#include <pulp/timebase/rational_time.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace pulp::playback {

struct TrackCompilePolicy {
    timeline::ItemId track_id;
    ProviderSelectorProgram provider;
    RendererStatePolicy state_policy = RendererStatePolicy::CarryByItemId;
};

struct ProgramCompileRequest {
    static constexpr std::size_t default_maximum_note_events_per_track = 1'000'000;

    std::shared_ptr<const timeline::Project> project;
    timeline::ItemId sequence_id;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map;
    /// The rate every frame position in the compiled program is expressed in,
    /// and the rate the caller will render that program at.
    ///
    /// Sample rate has two carriers here, so the rule between them is fixed:
    /// the compiled tempo map is authoritative and this field is a required
    /// declaration that must agree with it. The tempo map's segments already
    /// store frame anchors computed at its own rate, and the compiler holds it
    /// as an immutable shared_ptr with no access to the editable points, so
    /// this field cannot override it -- a differing value would not move a
    /// single frame, it would only describe the program wrongly.
    ///
    /// It is therefore not derived silently either. submit() rejects a request
    /// that leaves this unset, or whose value does not normalize-equal
    /// tempo_map->sample_rate(), with CompileErrorCode::InvalidRequest. Forcing
    /// the caller to state the rate is the point: a caller that compiles
    /// against a 48 kHz tempo map and then renders the result at 44.1 kHz is
    /// silently wrong today, and this turns that into a rejected submission.
    /// Compare normalized -- CompiledTempoMap stores its rate normalized, so a
    /// caller stating {96000, 2} means the same rate as a stored {48000, 1}.
    timebase::RationalRate sample_rate{0, 1};
    std::uint64_t document_revision = 0;
    DirtyTrackSet dirty;
    std::vector<TrackCompilePolicy> track_policies;
    std::shared_ptr<const DecodedAudioAssetPool> audio_assets;
    AudioRendererLimits audio_limits;
    AutomationPlaybackLimits automation_limits = AutomationPlaybackLimits::platform_defaults();
    std::uint64_t max_expanded_note_events = 1'000'000u;
    std::uint64_t max_expanded_clips = 1'000'000u;
    std::size_t maximum_note_events_per_track = default_maximum_note_events_per_track;
};

enum class CompileErrorCode : std::uint8_t {
    InvalidRequest,
    StaleRevision,
    ExecutorUnavailable,
    InvalidStructure,
    GenerationExhausted,
    CompilerAlreadyBound,
    AudioProgramInvalid,
    AutomationProgramInvalid,
    NestedSequenceUnsupported,
    ExpansionBudgetExceeded,
    NoteProgramCapacityExceeded,
    OfflineStretchFailed,
    // A nested clip needed trimming while its MIDI content carried controller
    // or expression lanes. Trimming has no defined answer for a lane: a point
    // before the retained window can still be the value sounding inside it, so
    // dropping it and carrying it are both wrong. Lowering such a clip requires
    // the chase rule that decides which point the window inherits.
    TrimmedMidiLaneUnsupported,
    // A clip carried controller or expression lanes into the note program,
    // which has no representation for them. Compiling it would emit the notes
    // and drop every authored controller point with nothing to read the loss
    // from, so the clip is refused instead. Distinct from the trimmed refusal
    // above: this one is answered by a renderer that chases and emits lane
    // values, whereas trimming still has no defined inherited value even once
    // that renderer exists.
    MidiExpressionLaneUnsupported,
};

struct CompileError {
    CompileErrorCode code = CompileErrorCode::InvalidRequest;
    timeline::ItemId item;
    std::uint64_t revision = 0;
    AudioRendererErrorCode audio_detail = AudioRendererErrorCode::InvalidAsset;
    OfflineStretchErrorCode offline_stretch_detail = OfflineStretchErrorCode::None;
};

struct CompileTicket {
    std::uint64_t revision = 0;
};

struct CompilerStatus {
    std::uint64_t latest_submitted_revision = 0;
    std::uint64_t latest_published_revision = 0;
    ProgramGeneration latest_published_generation = 0;
    std::uint64_t submitted_requests = 0;
    std::uint64_t coalesced_requests = 0;
    std::uint64_t rejected_requests = 0;
    bool busy = false;
    std::uint64_t active_tracks_completed = 0;
    bool has_error = false;
    CompileError last_error;
};

struct PlaybackProgramCompilerCore;

class PlaybackProgramCompiler {
  public:
    /// The store and executor must outlive every task they accept. Destroying
    /// this facade stops new submissions; accepted tasks retain shared compiler
    /// state and may finish without dereferencing the facade.
    /// Exactly one control thread submits requests; task execution may occur on
    /// any executor thread.
    PlaybackProgramCompiler(
        PlaybackProgramStore& store, CompileExecutor& executor,
        std::chrono::microseconds coalescing_window = std::chrono::milliseconds(10));
    ~PlaybackProgramCompiler();
    PlaybackProgramCompiler(const PlaybackProgramCompiler&) = delete;
    PlaybackProgramCompiler& operator=(const PlaybackProgramCompiler&) = delete;

    runtime::Result<CompileTicket, CompileError> submit(ProgramCompileRequest request);
    CompilerStatus status() const;

  private:
    std::shared_ptr<PlaybackProgramCompilerCore> core_;
};

} // namespace pulp::playback
