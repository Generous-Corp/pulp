#include <pulp/playback/program_compiler.hpp>

#include "budgeted_stable_merge.hpp"
#include "track_automation_compiler.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace pulp::playback {

struct PlaybackProgramCompilerCore;

class ProgramCompilerTask final : public CompileTask {
  public:
    explicit ProgramCompilerTask(std::shared_ptr<PlaybackProgramCompilerCore> core)
        : core_(std::move(core)) {}
    CompileTaskStatus run_slice(const CompileSliceBudget& budget) noexcept override;

  private:
    enum class Stage {
        Capture,
        CompileTracks,
        SortTrackNotes,
        SortTrackAudioIds,
        ValidateTrackAudioIds,
        CompileTrackTakeComp,
        SortTrackAudio,
        CompileTrackAutomation,
        FinalizeTrack,
        Link,
        Validate,
        Publish,
    };
    CompileTaskStatus fail(CompileError error) noexcept;
    void begin_track_automation();
    void begin_track_audio_link();

    std::shared_ptr<PlaybackProgramCompilerCore> core_;
    std::unique_ptr<ProgramCompileRequest> request_;
    const timeline::Sequence* sequence_ = nullptr;
    Stage stage_ = Stage::Capture;
    ProgramGeneration generation_ = 0;
    bool all_dirty_ = false;
    std::size_t track_index_ = 0;
    std::size_t clip_index_ = 0;
    std::size_t note_index_ = 0;
    bool clip_started_ = false;
    std::vector<timeline::ItemId> current_clip_ids_;
    std::vector<NoteProgramEvent> current_note_events_;
    std::vector<NoteProgramEvent> note_merge_buffer_;
    detail::BudgetedStableMergeState note_merge_;
    std::vector<AudioClipRendererProgram> current_audio_clips_;
    std::vector<timeline::ItemId> current_audio_ids_;
    std::vector<timeline::ItemId> audio_id_merge_buffer_;
    detail::BudgetedStableMergeState audio_id_merge_;
    std::size_t audio_id_validation_index_ = 1;
    std::vector<AudioClipRendererProgram> audio_merge_buffer_;
    detail::BudgetedStableMergeState audio_merge_;
    const timeline::TakeLane* current_take_lane_ = nullptr;
    bool current_track_frozen_ = false;
    std::size_t take_comp_index_ = 0;
    detail::TrackAutomationCompiler automation_compiler_;
    detail::CompiledTrackAutomation current_automation_;
    bool automation_compiler_started_ = false;
    std::uint64_t total_audio_clips_ = 0;
    std::vector<std::shared_ptr<const TrackProgram>> tracks_;
    std::vector<std::shared_ptr<const TrackProgram>> merge_buffer_;
    detail::BudgetedStableMergeState track_merge_;
    std::size_t validation_track_ = 0;
    std::size_t validation_clip_ = 0;
    std::size_t validation_note_ = 0;
};

struct PlaybackProgramCompilerCore
    : public std::enable_shared_from_this<PlaybackProgramCompilerCore> {
    PlaybackProgramCompilerCore(PlaybackProgramStore& store_in, CompileExecutor& executor_in,
                                std::chrono::microseconds window)
        : store(store_in), executor(executor_in), coalescing_window(window),
          bound(store.try_bind_compiler()) {
        accepting = bound;
        if (bound && store.live()) {
            status.latest_submitted_revision = store.live()->document_revision();
            status.latest_published_revision = store.live()->document_revision();
            status.latest_published_generation = store.live()->generation();
        } else if (!bound) {
            status.has_error = true;
            status.last_error = {CompileErrorCode::CompilerAlreadyBound, {}, 0};
        }
    }

    ~PlaybackProgramCompilerCore() {
        if (bound)
            store.unbind_compiler();
    }

    bool dispatch() {
        const bool accepted =
            executor.submit(std::make_unique<ProgramCompilerTask>(shared_from_this()),
                            std::chrono::steady_clock::now() + coalescing_window);
        if (!accepted)
            dispatch_rejected();
        return accepted;
    }

    void dispatch_rejected() {
        std::lock_guard lock(mutex);
        task_scheduled = false;
        pending.reset();
        status.busy = false;
        status.has_error = true;
        status.last_error = {
            CompileErrorCode::ExecutorUnavailable, {}, status.latest_submitted_revision};
        ++status.rejected_requests;
        status.latest_submitted_revision = status.latest_published_revision;
    }

    std::unique_ptr<ProgramCompileRequest> take_pending() {
        std::lock_guard lock(mutex);
        status.active_tracks_completed = 0;
        return std::move(pending);
    }

    void track_completed() {
        std::lock_guard lock(mutex);
        ++status.active_tracks_completed;
    }

    void finish(bool published, std::uint64_t revision, ProgramGeneration generation,
                CompileError error = {}) {
        bool reschedule = false;
        {
            std::lock_guard lock(mutex);
            task_scheduled = false;
            if (published) {
                status.latest_published_revision = revision;
                status.latest_published_generation = generation;
                status.has_error = false;
            } else if (error.code != CompileErrorCode::InvalidRequest || error.revision != 0) {
                status.has_error = true;
                status.last_error = error;
            }
            if (pending) {
                task_scheduled = true;
                reschedule = true;
            } else {
                status.busy = false;
            }
        }
        if (reschedule)
            (void)dispatch();
    }

    PlaybackProgramStore& store;
    CompileExecutor& executor;
    const std::chrono::microseconds coalescing_window;
    std::mutex mutex;
    std::unique_ptr<ProgramCompileRequest> pending;
    CompilerStatus status;
    bool task_scheduled = false;
    bool accepting = true;
    const bool bound;
};

void ProgramCompilerTask::begin_track_automation() {
    if (current_track_frozen_) {
        current_automation_ = {};
        stage_ = Stage::FinalizeTrack;
        return;
    }
    automation_compiler_started_ = false;
    stage_ = Stage::CompileTrackAutomation;
}

void ProgramCompilerTask::begin_track_audio_link() {
    if (current_audio_ids_.size() > 1) {
        audio_id_merge_.reset(audio_id_merge_buffer_);
        stage_ = Stage::SortTrackAudioIds;
    } else if (current_audio_clips_.size() > 1) {
        audio_merge_.reset(audio_merge_buffer_);
        stage_ = Stage::SortTrackAudio;
    } else {
        begin_track_automation();
    }
}

CompileTaskStatus ProgramCompilerTask::run_slice(const CompileSliceBudget& budget) noexcept {
    if (!request_) {
        request_ = core_->take_pending();
        if (!request_) {
            core_->finish(false, 0, 0);
            return CompileTaskStatus::Complete;
        }
        sequence_ = request_->project->find_sequence(request_->sequence_id);
        if (!sequence_)
            return fail({CompileErrorCode::InvalidStructure, request_->sequence_id,
                         request_->document_revision});
        if (sequence_->tracks().size() > request_->audio_limits.max_tracks)
            return fail({CompileErrorCode::AudioProgramInvalid, request_->sequence_id,
                         request_->document_revision, AudioRendererErrorCode::CapacityExceeded});
        const auto& live = core_->store.live();
        const auto next_generation = next_program_generation(live ? live->generation() : 0);
        if (!next_generation)
            return fail({CompileErrorCode::GenerationExhausted, {}, request_->document_revision});
        generation_ = next_generation.value();
        all_dirty_ = request_->dirty.all || !live ||
                     live->project_id() != request_->project->id() ||
                     live->sequence_id() != request_->sequence_id ||
                     live->tempo_map_owner().get() != request_->tempo_map.get() ||
                     live->audio_assets_owner().get() != request_->audio_assets.get() ||
                     live->audio_limits() != request_->audio_limits ||
                     live->automation_limits() != request_->automation_limits ||
                     live->tempo_map().sample_rate() != request_->tempo_map->sample_rate();
        tracks_.reserve(sequence_->tracks().size());
        merge_buffer_.reserve(sequence_->tracks().size());
        stage_ = Stage::CompileTracks;
    }

    std::size_t work = 0;
    while (std::chrono::steady_clock::now() < budget.deadline && work < budget.max_work_units) {
        if (stage_ == Stage::CompileTracks) {
            if (track_index_ == sequence_->tracks().size()) {
                track_merge_.reset(merge_buffer_);
                stage_ = Stage::Link;
                continue;
            }
            const auto& track = sequence_->tracks()[track_index_];
            const bool dirty =
                all_dirty_ || std::binary_search(request_->dirty.tracks.begin(),
                                                 request_->dirty.tracks.end(), track.id());
            if (!dirty) {
                const auto& live = core_->store.live();
                const auto* old = live ? live->find_track_owner(track.id()) : nullptr;
                if (!old)
                    return fail({CompileErrorCode::InvalidStructure, track.id(),
                                 request_->document_revision});
                if ((*old)->audio_program()) {
                    const auto count = (*old)->audio_program()->clips().size();
                    if (count > request_->audio_limits.max_clips - total_audio_clips_)
                        return fail({CompileErrorCode::AudioProgramInvalid, track.id(),
                                     request_->document_revision,
                                     AudioRendererErrorCode::CapacityExceeded});
                    total_audio_clips_ += count;
                }
                tracks_.push_back(*old);
                core_->track_completed();
                ++track_index_;
                ++work;
                continue;
            }
            if (clip_index_ == 0 && !clip_started_) {
                current_track_frozen_ = track.freeze().has_value();
                current_take_lane_ = !current_track_frozen_ && track.active_take_lane_id().valid()
                                         ? track.find_take_lane(track.active_take_lane_id())
                                         : nullptr;
                current_clip_ids_.reserve(
                    current_take_lane_ || current_track_frozen_ ? 0 : track.clips().size());
                const auto comp_count =
                    current_take_lane_ ? current_take_lane_->comp_segments().size() : 0;
                const auto remaining =
                    request_->audio_limits.max_clips -
                    std::min<std::uint64_t>(total_audio_clips_, request_->audio_limits.max_clips);
                const auto source_count =
                    current_track_frozen_
                        ? 1
                        : (current_take_lane_ ? comp_count : track.clips().size());
                const auto audio_capacity =
                    static_cast<std::size_t>(std::min<std::uint64_t>(source_count, remaining));
                current_audio_clips_.reserve(audio_capacity);
                current_audio_ids_.reserve(
                    current_take_lane_ || current_track_frozen_ ? 0 : audio_capacity);
                audio_id_merge_buffer_.reserve(
                    current_take_lane_ || current_track_frozen_ ? 0 : audio_capacity);
                audio_merge_buffer_.reserve(audio_capacity);
            }
            if (current_track_frozen_) {
                if (clip_index_ == 0) {
                    if (!request_->audio_assets)
                        return fail({CompileErrorCode::AudioProgramInvalid, track.id(),
                                     request_->document_revision,
                                     AudioRendererErrorCode::MissingDecodedAsset});
                    if (total_audio_clips_ >= request_->audio_limits.max_clips)
                        return fail({CompileErrorCode::AudioProgramInvalid, track.id(),
                                     request_->document_revision,
                                     AudioRendererErrorCode::CapacityExceeded});
                    auto compiled = compile_track_freeze_program(
                        track, *request_->project, *request_->tempo_map, *request_->audio_assets,
                        request_->audio_limits);
                    if (!compiled)
                        return fail({CompileErrorCode::AudioProgramInvalid, compiled.error().item,
                                     request_->document_revision, compiled.error().code});
                    ++total_audio_clips_;
                    current_audio_clips_.push_back(std::move(compiled).value());
                    clip_index_ = 1;
                    ++work;
                    continue;
                }
                stage_ = Stage::CompileTrackTakeComp;
                continue;
            }
            if (current_take_lane_) {
                stage_ = Stage::CompileTrackTakeComp;
                continue;
            }
            if (clip_index_ == track.clips().size()) {
                stage_ = Stage::CompileTrackTakeComp;
                continue;
            }

            const auto& clip = track.clips()[clip_index_];
            if (!clip_started_) {
                current_clip_ids_.push_back(clip.id());
                if (std::holds_alternative<timeline::MediaRef>(clip.content())) {
                    if (!request_->audio_assets)
                        return fail({CompileErrorCode::AudioProgramInvalid, clip.id(),
                                     request_->document_revision,
                                     AudioRendererErrorCode::MissingDecodedAsset});
                    if (total_audio_clips_ >= request_->audio_limits.max_clips)
                        return fail({CompileErrorCode::AudioProgramInvalid, clip.id(),
                                     request_->document_revision,
                                     AudioRendererErrorCode::CapacityExceeded});
                    auto compiled =
                        compile_audio_clip_program(clip, *request_->project, *request_->tempo_map,
                                                   *request_->audio_assets, request_->audio_limits);
                    if (!compiled)
                        return fail({CompileErrorCode::AudioProgramInvalid, compiled.error().item,
                                     request_->document_revision, compiled.error().code});
                    ++total_audio_clips_;
                    current_audio_clips_.push_back(std::move(compiled).value());
                    current_audio_ids_.push_back(clip.id());
                }
                clip_started_ = true;
            }
            const auto* notes = std::get_if<timeline::NoteContent>(&clip.content());
            if (!notes || note_index_ == notes->notes().size()) {
                note_index_ = 0;
                clip_started_ = false;
                ++clip_index_;
                ++work;
                continue;
            }
            if (clip.time_anchor() != timeline::ClipTimeAnchor::Musical)
                return fail(
                    {CompileErrorCode::InvalidStructure, clip.id(), request_->document_revision});
            const auto& note = notes->notes()[note_index_++];
            const auto note_end = note.start + note.duration;
            if (note.start.value < 0 || note_end > timebase::TickPosition{clip.duration().value})
                return fail(
                    {CompileErrorCode::InvalidStructure, note.id, request_->document_revision});
            const auto start_tick = clip.start() + timebase::TickDuration{note.start.value};
            const auto end_tick = clip.start() + timebase::TickDuration{note_end.value};
            const auto start_sample = request_->tempo_map->ticks_to_samples(start_tick);
            const auto end_sample = request_->tempo_map->ticks_to_samples(end_tick);
            if (end_sample <= start_sample)
                return fail(
                    {CompileErrorCode::InvalidStructure, note.id, request_->document_revision});
            current_note_events_.push_back({start_sample, start_tick, clip.id(), note.id,
                                            note.velocity, note.pitch, note.channel,
                                            NoteProgramEventKind::On});
            current_note_events_.push_back({end_sample, end_tick, clip.id(), note.id, note.velocity,
                                            note.pitch, note.channel, NoteProgramEventKind::Off});
            ++work;
            continue;
        }
        if (stage_ == Stage::CompileTrackTakeComp) {
            if (!current_take_lane_ ||
                take_comp_index_ == current_take_lane_->comp_segments().size()) {
                if (current_note_events_.size() > 1) {
                    note_merge_buffer_.reserve(current_note_events_.size());
                    note_merge_.reset(note_merge_buffer_);
                    stage_ = Stage::SortTrackNotes;
                } else {
                    begin_track_audio_link();
                }
                continue;
            }
            if (!request_->audio_assets)
                return fail({CompileErrorCode::AudioProgramInvalid, current_take_lane_->id(),
                             request_->document_revision,
                             AudioRendererErrorCode::MissingDecodedAsset});
            if (total_audio_clips_ >= request_->audio_limits.max_clips)
                return fail({CompileErrorCode::AudioProgramInvalid, current_take_lane_->id(),
                             request_->document_revision,
                             AudioRendererErrorCode::CapacityExceeded});
            auto compiled = compile_take_comp_segment_program(
                *current_take_lane_, take_comp_index_, *request_->project, *request_->tempo_map,
                *request_->audio_assets, request_->audio_limits);
            if (!compiled)
                return fail({CompileErrorCode::AudioProgramInvalid, compiled.error().item,
                             request_->document_revision, compiled.error().code});
            ++total_audio_clips_;
            ++take_comp_index_;
            current_audio_clips_.push_back(std::move(compiled).value());
            ++work;
            continue;
        }
        if (stage_ == Stage::SortTrackNotes) {
            const auto step = note_merge_.step(
                current_note_events_, note_merge_buffer_, note_program_event_less);
            work += step.work_units;
            if (step.complete)
                begin_track_audio_link();
            continue;
        }
        if (stage_ == Stage::SortTrackAudioIds) {
            const auto step = audio_id_merge_.step(
                current_audio_ids_, audio_id_merge_buffer_,
                [](timeline::ItemId lhs, timeline::ItemId rhs) { return lhs < rhs; });
            work += step.work_units;
            if (step.complete) {
                audio_id_validation_index_ = 1;
                stage_ = Stage::ValidateTrackAudioIds;
            }
            continue;
        }
        if (stage_ == Stage::ValidateTrackAudioIds) {
            if (audio_id_validation_index_ < current_audio_ids_.size()) {
                const auto id = current_audio_ids_[audio_id_validation_index_];
                const bool duplicate = id == current_audio_ids_[audio_id_validation_index_ - 1];
                ++audio_id_validation_index_;
                ++work;
                if (duplicate)
                    return fail({CompileErrorCode::AudioProgramInvalid, id,
                                 request_->document_revision,
                                 AudioRendererErrorCode::InvalidIdentity});
                continue;
            }
            audio_merge_.reset(audio_merge_buffer_);
            stage_ = Stage::SortTrackAudio;
            continue;
        }
        if (stage_ == Stage::SortTrackAudio) {
            const auto step = audio_merge_.step(
                current_audio_clips_, audio_merge_buffer_,
                [](const AudioClipRendererProgram& lhs, const AudioClipRendererProgram& rhs) {
                    return std::tuple(lhs.timeline_start, lhs.source_kind, lhs.id.value,
                                      lhs.source_ordinal) <
                           std::tuple(rhs.timeline_start, rhs.source_kind, rhs.id.value,
                                      rhs.source_ordinal);
                });
            work += step.work_units;
            if (step.complete)
                begin_track_automation();
            continue;
        }
        if (stage_ == Stage::CompileTrackAutomation) {
            if (!automation_compiler_started_) {
                const auto& track = sequence_->tracks()[track_index_];
                automation_compiler_.reset(track, request_->tempo_map, generation_,
                                           request_->automation_limits);
                automation_compiler_started_ = true;
            }
            auto step = automation_compiler_.step();
            ++work;
            if (!step)
                return fail({CompileErrorCode::AutomationProgramInvalid, step.error().item,
                             request_->document_revision});
            if (step.value() == detail::TrackAutomationCompileStatus::Complete) {
                current_automation_ = automation_compiler_.take_result();
                stage_ = Stage::FinalizeTrack;
            }
            continue;
        }
        if (stage_ == Stage::FinalizeTrack) {
            const auto& track = sequence_->tracks()[track_index_];
            ProviderSelectorProgram provider;
            RendererStatePolicy state_policy = RendererStatePolicy::CarryByItemId;
            const auto& live = core_->store.live();
            if (live && live->project_id() == request_->project->id() &&
                live->sequence_id() == request_->sequence_id) {
                if (const auto* prior = live->find_track(track.id())) {
                    provider = prior->provider();
                    state_policy = prior->state_policy();
                }
            }
            const auto policy = std::lower_bound(
                request_->track_policies.begin(), request_->track_policies.end(), track.id(),
                [](const TrackCompilePolicy& value, timeline::ItemId id) {
                    return value.track_id < id;
                });
            if (policy != request_->track_policies.end() && policy->track_id == track.id()) {
                provider = policy->provider;
                state_policy = policy->state_policy;
            }
            std::shared_ptr<const AudioTrackRendererProgram> audio_program;
            if (!current_audio_clips_.empty()) {
                audio_program = std::shared_ptr<const AudioTrackRendererProgram>(
                    new AudioTrackRendererProgram(track.id(), std::move(current_audio_clips_)));
            }
            tracks_.push_back(std::shared_ptr<const TrackProgram>(new TrackProgram(
                track.id(), generation_, provider, state_policy, std::move(current_clip_ids_),
                std::move(current_note_events_), std::move(audio_program),
                std::move(current_automation_.ordered_device_placement_ids),
                std::move(current_automation_.program))));
            core_->track_completed();
            current_clip_ids_.clear();
            current_note_events_.clear();
            current_audio_clips_.clear();
            current_audio_ids_.clear();
            current_take_lane_ = nullptr;
            current_track_frozen_ = false;
            take_comp_index_ = 0;
            current_automation_ = {};
            automation_compiler_started_ = false;
            clip_index_ = 0;
            note_index_ = 0;
            clip_started_ = false;
            ++track_index_;
            ++work;
            stage_ = Stage::CompileTracks;
            continue;
        }
        if (stage_ == Stage::Link) {
            const auto step = track_merge_.step(
                tracks_, merge_buffer_, [](const auto& lhs, const auto& rhs) {
                    return lhs->id() < rhs->id();
                });
            work += step.work_units;
            if (step.complete)
                stage_ = Stage::Validate;
            continue;
        }
        if (stage_ == Stage::Validate) {
            if (validation_track_ == tracks_.size()) {
                stage_ = Stage::Publish;
                continue;
            }
            const auto& track = tracks_[validation_track_];
            if (validation_clip_ == 0 &&
                (!track->id().valid() || track->generation() == 0 ||
                 (validation_track_ && tracks_[validation_track_ - 1]->id() == track->id())))
                return fail(
                    {CompileErrorCode::InvalidStructure, track->id(), request_->document_revision});
            const auto clips = track->ordered_clip_ids();
            if (validation_clip_ < clips.size()) {
                const auto id = clips[validation_clip_++];
                ++work;
                if (!id.valid())
                    return fail(
                        {CompileErrorCode::InvalidStructure, id, request_->document_revision});
                continue;
            }
            const auto notes = track->arrangement_note_events();
            if (validation_note_ < notes.size()) {
                const auto& event = notes[validation_note_];
                const bool malformed =
                    !event.clip_id.valid() || !event.note_id.valid() || event.pitch > 127 ||
                    event.channel > 15 ||
                    static_cast<unsigned>(event.kind) >
                        static_cast<unsigned>(NoteProgramEventKind::On) ||
                    (validation_note_ != 0 &&
                     note_program_event_less(event, notes[validation_note_ - 1]));
                ++validation_note_;
                ++work;
                if (malformed)
                    return fail({CompileErrorCode::InvalidStructure, event.note_id,
                                 request_->document_revision});
            } else {
                validation_clip_ = 0;
                validation_note_ = 0;
                ++validation_track_;
                ++work;
            }
            continue;
        }
        auto program = std::shared_ptr<const PlaybackProgram>(
            new PlaybackProgram(generation_, request_->document_revision, request_->project->id(),
                                request_->sequence_id, request_->tempo_map, request_->audio_assets,
                                request_->audio_limits, request_->automation_limits,
                                std::move(tracks_)));
        core_->store.publish(std::move(program));
        core_->finish(true, request_->document_revision, generation_);
        return CompileTaskStatus::Complete;
    }
    return CompileTaskStatus::Pending;
}

CompileTaskStatus ProgramCompilerTask::fail(CompileError error) noexcept {
    core_->finish(false, error.revision, 0, error);
    return CompileTaskStatus::Complete;
}

PlaybackProgramCompiler::PlaybackProgramCompiler(PlaybackProgramStore& store,
                                                 CompileExecutor& executor,
                                                 std::chrono::microseconds coalescing_window)
    : core_(std::make_shared<PlaybackProgramCompilerCore>(store, executor, coalescing_window)) {}

PlaybackProgramCompiler::~PlaybackProgramCompiler() {
    std::lock_guard lock(core_->mutex);
    core_->accepting = false;
}

runtime::Result<CompileTicket, CompileError>
PlaybackProgramCompiler::submit(ProgramCompileRequest request) {
    const auto submitted_revision = request.document_revision;
    auto reject = [this](CompileError error) -> runtime::Result<CompileTicket, CompileError> {
        std::lock_guard lock(core_->mutex);
        ++core_->status.rejected_requests;
        core_->status.has_error = true;
        core_->status.last_error = error;
        return runtime::Err(error);
    };
    if (!request.project || !request.sequence_id.valid() || !request.tempo_map ||
        request.document_revision == 0 ||
        !request.automation_limits.valid() ||
        (!request.dirty.all && request.dirty.tracks.empty() && request.track_policies.empty()))
        return reject({CompileErrorCode::InvalidRequest, {}, request.document_revision});
    const auto* sequence = request.project->find_sequence(request.sequence_id);
    if (!sequence)
        return reject(
            {CompileErrorCode::InvalidRequest, request.sequence_id, request.document_revision});
    std::sort(request.dirty.tracks.begin(), request.dirty.tracks.end());
    request.dirty.tracks.erase(
        std::unique(request.dirty.tracks.begin(), request.dirty.tracks.end()),
        request.dirty.tracks.end());
    for (const auto id : request.dirty.tracks)
        if (!id.valid() || !sequence->find_track(id))
            return reject({CompileErrorCode::InvalidRequest, id, request.document_revision});
    std::sort(request.track_policies.begin(), request.track_policies.end(),
              [](const TrackCompilePolicy& a, const TrackCompilePolicy& b) {
                  return a.track_id < b.track_id;
              });
    for (std::size_t i = 0; i < request.track_policies.size(); ++i) {
        const auto& policy = request.track_policies[i];
        if (!policy.track_id.valid() || !sequence->find_track(policy.track_id) ||
            !policy.provider.available(policy.provider.selected) ||
            policy.provider.selected != ProviderKind::Arrangement ||
            policy.provider.available_mask != 1u ||
            static_cast<unsigned>(policy.state_policy) >
                static_cast<unsigned>(RendererStatePolicy::CarryByItemId) ||
            (i && request.track_policies[i - 1].track_id == policy.track_id))
            return reject(
                {CompileErrorCode::InvalidRequest, policy.track_id, request.document_revision});
        request.dirty.tracks.push_back(policy.track_id);
    }
    std::sort(request.dirty.tracks.begin(), request.dirty.tracks.end());
    request.dirty.tracks.erase(
        std::unique(request.dirty.tracks.begin(), request.dirty.tracks.end()),
        request.dirty.tracks.end());

    bool dispatch = false;
    {
        std::lock_guard lock(core_->mutex);
        if (!core_->accepting)
            return runtime::Err(
                core_->bound
                    ? CompileError{CompileErrorCode::InvalidRequest, {}, request.document_revision}
                    : CompileError{
                          CompileErrorCode::CompilerAlreadyBound, {}, request.document_revision});
        if (request.document_revision <= core_->status.latest_submitted_revision) {
            ++core_->status.rejected_requests;
            const CompileError error{
                CompileErrorCode::StaleRevision, {}, request.document_revision};
            core_->status.has_error = true;
            core_->status.last_error = error;
            return runtime::Err(error);
        }
        ++core_->status.submitted_requests;
        core_->status.latest_submitted_revision = request.document_revision;
        if (core_->pending) {
            ++core_->status.coalesced_requests;
            auto dirty = std::move(core_->pending->dirty.tracks);
            dirty.insert(dirty.end(), request.dirty.tracks.begin(), request.dirty.tracks.end());
            std::sort(dirty.begin(), dirty.end());
            dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
            request.dirty.all = request.dirty.all || core_->pending->dirty.all ||
                                core_->pending->sequence_id != request.sequence_id ||
                                core_->pending->project->id() != request.project->id() ||
                                core_->pending->tempo_map.get() != request.tempo_map.get() ||
                                core_->pending->audio_assets.get() != request.audio_assets.get() ||
                                core_->pending->audio_limits != request.audio_limits;
            request.dirty.tracks = std::move(dirty);

            const bool same_policy_domain =
                core_->pending->project->id() == request.project->id() &&
                core_->pending->sequence_id == request.sequence_id;
            if (same_policy_domain) {
                // Policies are sparse deltas. Preserve pending deltas that have
                // not reached publication; newest delta for a track wins.
                auto policies = std::move(core_->pending->track_policies);
                for (const auto& incoming : request.track_policies) {
                    const auto found =
                        std::lower_bound(policies.begin(), policies.end(), incoming.track_id,
                                         [](const TrackCompilePolicy& value, timeline::ItemId id) {
                                             return value.track_id < id;
                                         });
                    if (found != policies.end() && found->track_id == incoming.track_id)
                        *found = incoming;
                    else
                        policies.insert(found, incoming);
                }
                request.track_policies = std::move(policies);
            }
        }
        core_->pending = std::make_unique<ProgramCompileRequest>(std::move(request));
        core_->status.busy = true;
        if (!core_->task_scheduled) {
            core_->task_scheduled = true;
            dispatch = true;
        }
    }
    if (dispatch && !core_->dispatch())
        return runtime::Err(
            CompileError{CompileErrorCode::ExecutorUnavailable, {}, submitted_revision});
    return runtime::Ok(CompileTicket{submitted_revision});
}

CompilerStatus PlaybackProgramCompiler::status() const {
    std::lock_guard lock(core_->mutex);
    return core_->status;
}

} // namespace pulp::playback
