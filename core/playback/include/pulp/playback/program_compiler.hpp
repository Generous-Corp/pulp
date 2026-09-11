#pragma once

#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/compile_context_registry.hpp>
#include <pulp/playback/compile_executor.hpp>
#include <pulp/playback/dirty_track_resolver.hpp>
#include <pulp/playback/offline_stretch_artifact.hpp>
#include <pulp/playback/program.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::timeline {
struct CommitResult;
}

namespace pulp::playback {

struct TrackCompilePolicy {
    timeline::ItemId track_id;
    ProviderSelectorProgram provider;
    RendererStatePolicy state_policy = RendererStatePolicy::CarryByItemId;
};

/// One committed document delta lowered through the compile-context registry.
///
/// This is the production path from sequence-owned context edits to exact
/// track recompilation. Construction binds the dirty set to an exact snapshot
/// pointer and document revision and copies an immutable registry generation.
/// The compiler rejects a request whose project or revision does not match.
struct CompileInvalidationInput {
    CompileInvalidationInput(std::shared_ptr<const CompileContextRegistry> registry,
                             const timeline::CommitResult& committed);

    /// Pin registry state without claiming a document delta. Use only with
    /// request.dirty.all for an initial full compile, a registry-only refresh,
    /// or full adoption of an already-live document revision.
    static CompileInvalidationInput baseline(std::shared_ptr<const CompileContextRegistry> registry,
                                             std::shared_ptr<const timeline::Project> snapshot,
                                             std::uint64_t document_revision);

    std::uint64_t captured_registry_revision() const noexcept {
        return captured_registry_revision_;
    }
    const void* captured_registry_generation() const noexcept {
        return registry_generation_.get();
    }
    const CompileContextRegistry* registry_snapshot() const noexcept {
        return registry_snapshot_.get();
    }
    const std::shared_ptr<const CompileContextRegistry>& registry_snapshot_owner() const noexcept {
        return registry_snapshot_;
    }
    std::shared_ptr<const void> captured_registry_generation_owner() const noexcept {
        return registry_generation_;
    }

  private:
    CompileInvalidationInput(std::shared_ptr<const CompileContextRegistry> registry,
                             std::shared_ptr<const timeline::Project> snapshot,
                             std::uint64_t document_revision, timeline::DirtySet committed,
                             std::shared_ptr<const timeline::Project> predecessor_snapshot,
                             bool baseline);
    friend class PlaybackProgramCompiler;
    friend struct PlaybackProgramCompilerCore;
    timeline::DirtySet committed_;
    std::shared_ptr<const timeline::Project> snapshot_;
    std::shared_ptr<const timeline::Project> predecessor_snapshot_;
    std::uint64_t document_revision_ = 0;
    std::shared_ptr<const detail::ContextRegistryGeneration> registry_generation_;
    std::shared_ptr<const CompileContextRegistry> registry_snapshot_;
    std::uint64_t captured_registry_revision_ = 0;
    bool baseline_ = false;
};

struct ProgramCompileRequest {
    static constexpr std::size_t default_maximum_note_events_per_track = 1'000'000;

    std::shared_ptr<const timeline::Project> project;
    timeline::ItemId sequence_id;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map;
    /// The render rate requested by the caller. Submission requires its normalized
    /// value to match tempo_map so compilation cannot mix two sample domains.
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
    // Appended to preserve positional aggregate initialization of the original
    // request fields. Snapshot and revision must match this request.
    std::optional<CompileInvalidationInput> invalidation;
    // Registry used by manual/full compilation callers that do not provide a
    // transaction-backed invalidation input. submit() captures a private
    // immutable copy plus the original generation/revision watermark before it
    // queues work. When invalidation is present its earlier captured snapshot
    // is authoritative instead.
    std::shared_ptr<const CompileContextRegistry> content_compilers;
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
    /// The arrangement authors more controller values than one program may
    /// carry. Bounded like every other expansion so a pathological document
    /// cannot make compilation unbounded.
    MidiExpressionLaneBudgetExceeded,
    // A nested SequenceRef trims a MIDI leaf whose owner has an authored groove
    // that does not state canonical no-feel. The flatten-first path cannot
    // decide whether displaced events outside the retained source window should
    // chase, clip, or disappear, so it refuses rather than leaking events.
    TrimmedGrooveUnsupported,
    // A nested SequenceRef exposes only a window of registered content. The
    // current hook input has no authored source-window offset, so compiling the
    // shortened leaf would restart any stateful pattern phase at the retained
    // boundary. Refuse until that provenance is part of the renderer contract.
    TrimmedRegisteredContentUnsupported,
    UnresolvedRegisteredContent,
    RegisteredContentCompileFailed,
    RegisteredContentFragmentQuotaExceeded,
    // A nested child track carries a stereo balance. Flattening folds the child
    // into the parent track, and a clip has no pan of its own, so the only
    // panner left in the result is the parent's — which also serves every other
    // clip on that track. Folding the child's pan into it would repan unrelated
    // material, and dropping it would silently centre the child, so the balance
    // is refused until a flattened leaf can carry its own placement.
    NestedMixerPanUnsupported,
    // A composed nested gain had nowhere to land. Gain composes by multiplying
    // into the flattened leaf's own clip gain, which only reaches a renderer
    // for content that reads it; note, registered and opaque leaves compile to
    // events no renderer scales by clip gain. Refuse rather than fold a child
    // fader into a value nothing will read.
    NestedGainSinkUnsupported,
    // A SequenceRef placement's fade had nowhere to land. The envelope itself
    // composes: it travels beside each flattened leaf as the ramp it is, and
    // the leaf is read at its own position within it. But a fade is a
    // time-varying gain, so it needs the sink a static gain needs, and the leaf
    // kinds that have none are the same ones NestedGainSinkUnsupported names.
    // Refuse rather than play such a leaf at full level through an envelope the
    // author wrote. Only a leaf a ramp actually reaches refuses; one lying
    // wholly past a ramp reads unity and compiles.
    NestedPlacementFadeUnsupported,
    // A nested child track is frozen. Freeze substitutes a sealed rendered
    // artifact for everything the track would otherwise play, which is why the
    // top-level walk returns Freeze content and discards the arrangement. The
    // nested walk has no such substitution: it descends into the child's clips,
    // so honouring the freeze would mean emitting the artifact in place of
    // material the walk is already reading, and ignoring it would play the very
    // arrangement the author froze. Refuse until a flattened leaf can carry
    // rendered media standing in for a whole child track.
    NestedFrozenTrackUnsupported,
    // A nested child track has a take lane selected. An active lane substitutes
    // its comped takes for the arrangement, exactly as freeze substitutes an
    // artifact, and the nested walk reads the arrangement instead. Playing the
    // arrangement would sound the take the author deselected, so the selection
    // is refused rather than silently inverted. Dormant lanes do not refuse:
    // an unselected lane changes nothing at either level.
    NestedActiveTakeUnsupported,
    // A nested child track carries a device chain. A device is bound to the
    // track it processes, and flattening dissolves the child track entirely:
    // its clips become leaves on the parent, whose own chain already serves
    // every other clip there. Running the child's devices over the parent
    // would process unrelated material, and dropping them would play the
    // child dry, so the chain is refused. Lifting it needs a sub-bus a
    // flattened group can keep its own processing on, not a wider flatten.
    NestedDeviceChainUnsupported,
    // A nested child track carries automation lanes. A lane is a curve over
    // the track's own timeline, and the flattened leaf has nowhere to hold a
    // curve: ClipPlaybackProperties::gain_linear is a scalar, so even the one
    // lane that could compose has no time-varying sink. Refuse rather than
    // freeze a moving value at a single point. A per-clip automation sink is
    // the missing construct; the pan and MIDI-gain lanes are not waiting on
    // it, because they have no destination at any level and can only ever be
    // declared intended.
    NestedAutomationLaneUnsupported,
    // A nested SequenceRef trims a media leaf whose content conforms to the
    // timeline. Both conform kinds break differently under a partial view.
    // Resample maps source to timeline by tick phase, while the nested trim
    // path advances a raw source-frame offset from elapsed samples, so a left
    // trim starts the clip at the wrong audio. Stretch keys its rendered
    // artifact to the clip's own authored tick range, and a trimmed window is
    // not that range. Refuse until the renderer owns a conform-aware
    // source-range mapping and a windowed stretch artifact.
    NestedConformedTrimUnsupported,
    // A leaf clip inside a nested sequence is absolute-anchored while the
    // nesting that reaches it is musical. Flattening has to place that leaf on
    // the owner's musical timeline, but its position is defined in samples and
    // must stay fixed as tempo moves, so the result would have to be musical
    // and absolute at once. The lowered program has no such hybrid domain to
    // write. Distinct from the placement guard above, which a SequenceRef clip
    // cannot reach: this one a document can genuinely author. What it waits on
    // is a product decision about whether nesting re-anchors such a leaf or
    // preserves it, not only a renderer construct.
    NestedAbsoluteChildUnsupported,
};

struct CompileError {
    CompileErrorCode code = CompileErrorCode::InvalidRequest;
    timeline::ItemId item;
    std::uint64_t revision = 0;
    AudioRendererErrorCode audio_detail = AudioRendererErrorCode::InvalidAsset;
    OfflineStretchErrorCode offline_stretch_detail = OfflineStretchErrorCode::None;
    ContentFragmentErrorCode content_fragment_detail = ContentFragmentErrorCode::RendererFailed;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
};

struct CompileTicket {
    std::uint64_t revision = 0;
    std::uint64_t submission_epoch = 0;
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
    // Compiler-instance-scoped terminal watermark. `>= ticket.epoch` means the
    // ticket published or was superseded; equality means that exact ticket was
    // the instance's latest successful publication.
    std::uint64_t latest_submitted_epoch = 0;
    std::uint64_t latest_published_epoch = 0;
    /// Partition of `active_tracks_completed` for the request currently being
    /// compiled, or for the last one if the compiler is idle. A track is
    /// `reused` when the dirty set spared it and its program was carried over
    /// from the live program untouched; it is `recompiled` when a fresh
    /// TrackProgram was built for it. The two always sum to
    /// `active_tracks_completed`.
    ///
    /// This is the direct measure of how much work an incremental compile did.
    /// Callers that need to know an edit stayed incremental should read these
    /// rather than infer it from how long the compile took, which measures the
    /// host as much as the compiler.
    std::uint64_t active_tracks_recompiled = 0;
    std::uint64_t active_tracks_reused = 0;

    /// Clear the three track counters together, so the partition can never be
    /// left describing a previous request.
    void clear_active_track_counts() {
        active_tracks_completed = 0;
        active_tracks_recompiled = 0;
        active_tracks_reused = 0;
    }

    /// Record one finished track on whichever side of the partition it fell.
    void count_track_completed(bool reused) {
        ++active_tracks_completed;
        ++(reused ? active_tracks_reused : active_tracks_recompiled);
    }
};

struct PlaybackProgramCompilerCore;

class PlaybackProgramCompiler {
  public:
    /// The store and executor must outlive every task they accept. Destroying
    /// this facade stops new submissions; accepted tasks retain shared compiler
    /// state and may finish without dereferencing the facade. Tickets and status
    /// epochs are scoped to this compiler instance; destroying the facade
    /// forfeits observation of any retained task's completion.
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
