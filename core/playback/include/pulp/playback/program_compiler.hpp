#pragma once

#include <pulp/playback/compile_executor.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace pulp::playback {

struct DirtyTrackSet {
    bool all = false;
    std::vector<timeline::ItemId> tracks;
};

struct TrackCompilePolicy {
    timeline::ItemId track_id;
    ProviderSelectorProgram provider;
    RendererStatePolicy state_policy = RendererStatePolicy::CarryByItemId;
};

struct ProgramCompileRequest {
    std::shared_ptr<const timeline::Project> project;
    timeline::ItemId sequence_id;
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map;
    std::uint64_t document_revision = 0;
    DirtyTrackSet dirty;
    std::vector<TrackCompilePolicy> track_policies;
    std::shared_ptr<const DecodedAudioAssetPool> audio_assets;
    AudioRendererLimits audio_limits;
    AutomationPlaybackLimits automation_limits = AutomationPlaybackLimits::platform_defaults();
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
};

struct CompileError {
    CompileErrorCode code = CompileErrorCode::InvalidRequest;
    timeline::ItemId item;
    std::uint64_t revision = 0;
    AudioRendererErrorCode audio_detail = AudioRendererErrorCode::InvalidAsset;
};

struct CompileTicket { std::uint64_t revision = 0; };

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
    PlaybackProgramCompiler(PlaybackProgramStore& store, CompileExecutor& executor,
                            std::chrono::microseconds coalescing_window =
                                std::chrono::milliseconds(10));
    ~PlaybackProgramCompiler();
    PlaybackProgramCompiler(const PlaybackProgramCompiler&) = delete;
    PlaybackProgramCompiler& operator=(const PlaybackProgramCompiler&) = delete;

    runtime::Result<CompileTicket, CompileError> submit(ProgramCompileRequest request);
    CompilerStatus status() const;

  private:
    std::shared_ptr<PlaybackProgramCompilerCore> core_;
};

} // namespace pulp::playback
