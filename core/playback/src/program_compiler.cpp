#include <pulp/playback/program_compiler.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace pulp::playback {

struct PlaybackProgramCompilerCore;

class ProgramCompilerTask final : public CompileTask {
  public:
    explicit ProgramCompilerTask(std::shared_ptr<PlaybackProgramCompilerCore> core)
        : core_(std::move(core)) {}
    CompileTaskStatus run_slice(const CompileSliceBudget& budget) noexcept override;

  private:
    enum class Stage { Capture, CompileTracks, Link, Validate, Publish };
    CompileTaskStatus fail(CompileError error) noexcept;

    std::shared_ptr<PlaybackProgramCompilerCore> core_;
    std::unique_ptr<ProgramCompileRequest> request_;
    const timeline::Sequence* sequence_ = nullptr;
    Stage stage_ = Stage::Capture;
    ProgramGeneration generation_ = 0;
    bool all_dirty_ = false;
    std::size_t track_index_ = 0;
    std::size_t clip_index_ = 0;
    std::vector<timeline::ItemId> current_clip_ids_;
    std::vector<std::shared_ptr<const TrackProgram>> tracks_;
    std::vector<std::shared_ptr<const TrackProgram>> merge_buffer_;
    std::size_t merge_width_ = 1;
    std::size_t merge_left_ = 0;
    std::size_t merge_mid_ = 0;
    std::size_t merge_right_ = 0;
    std::size_t merge_i_ = 0;
    std::size_t merge_j_ = 0;
    bool merge_pair_active_ = false;
    bool clearing_merge_source_ = false;
    std::size_t validation_track_ = 0;
    std::size_t validation_clip_ = 0;
};

struct PlaybackProgramCompilerCore :
    public std::enable_shared_from_this<PlaybackProgramCompilerCore> {
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
        if (bound) store.unbind_compiler();
    }

    bool dispatch() {
        const bool accepted = executor.submit(std::make_unique<ProgramCompilerTask>(shared_from_this()),
            std::chrono::steady_clock::now() + coalescing_window);
        if (!accepted) dispatch_rejected();
        return accepted;
    }

    void dispatch_rejected() {
        std::lock_guard lock(mutex);
        task_scheduled = false;
        pending.reset();
        status.busy = false;
        status.has_error = true;
        status.last_error = {CompileErrorCode::ExecutorUnavailable, {},
                             status.latest_submitted_revision};
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
        if (reschedule) (void)dispatch();
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
        const auto& live = core_->store.live();
        const auto next_generation = next_program_generation(live ? live->generation() : 0);
        if (!next_generation)
            return fail({CompileErrorCode::GenerationExhausted, {}, request_->document_revision});
        generation_ = next_generation.value();
        all_dirty_ = request_->dirty.all || !live || live->project_id() != request_->project->id() ||
                     live->sequence_id() != request_->sequence_id ||
                     live->tempo_map_owner().get() != request_->tempo_map.get() ||
                     live->tempo_map().sample_rate() != request_->tempo_map->sample_rate();
        tracks_.reserve(sequence_->tracks().size());
        merge_buffer_.reserve(sequence_->tracks().size());
        stage_ = Stage::CompileTracks;
    }

    std::size_t work = 0;
    while (std::chrono::steady_clock::now() < budget.deadline && work < budget.max_work_units) {
        if (stage_ == Stage::CompileTracks) {
            if (track_index_ == sequence_->tracks().size()) {
                stage_ = Stage::Link;
                continue;
            }
            const auto& track = sequence_->tracks()[track_index_];
            const bool dirty = all_dirty_ || std::binary_search(
                request_->dirty.tracks.begin(), request_->dirty.tracks.end(), track.id());
            if (!dirty) {
                const auto& live = core_->store.live();
                const auto* old = live ? live->find_track_owner(track.id()) : nullptr;
                if (!old)
                    return fail({CompileErrorCode::InvalidStructure, track.id(),
                                 request_->document_revision});
                tracks_.push_back(*old);
                core_->track_completed();
                ++track_index_;
                ++work;
                continue;
            }
            if (clip_index_ == 0) current_clip_ids_.reserve(track.clips().size());
            while (clip_index_ < track.clips().size() && work < budget.max_work_units &&
                   std::chrono::steady_clock::now() < budget.deadline) {
                current_clip_ids_.push_back(track.clips()[clip_index_++].id());
                ++work;
            }
            if (clip_index_ == track.clips().size()) {
                if (work >= budget.max_work_units ||
                    std::chrono::steady_clock::now() >= budget.deadline)
                    return CompileTaskStatus::Pending;
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
                tracks_.push_back(std::shared_ptr<const TrackProgram>(new TrackProgram(
                    track.id(), generation_, provider, state_policy,
                    std::move(current_clip_ids_))));
                core_->track_completed();
                current_clip_ids_.clear();
                clip_index_ = 0;
                ++track_index_;
                ++work;
            }
            continue;
        }
        if (stage_ == Stage::Link) {
            if (tracks_.size() <= 1 || merge_width_ >= tracks_.size()) {
                stage_ = Stage::Validate;
                continue;
            }
            if (clearing_merge_source_) {
                merge_buffer_.pop_back();
                ++work;
                if (merge_buffer_.empty()) {
                    clearing_merge_source_ = false;
                    merge_width_ *= 2;
                    merge_left_ = 0;
                }
                continue;
            }
            if (merge_left_ >= tracks_.size()) {
                tracks_.swap(merge_buffer_);
                clearing_merge_source_ = true;
                continue;
            }
            if (!merge_pair_active_) {
                merge_mid_ = std::min(merge_left_ + merge_width_, tracks_.size());
                merge_right_ = std::min(merge_left_ + 2 * merge_width_, tracks_.size());
                merge_i_ = merge_left_;
                merge_j_ = merge_mid_;
                merge_pair_active_ = true;
            }
            if (merge_i_ < merge_mid_ &&
                (merge_j_ >= merge_right_ || tracks_[merge_i_]->id() <= tracks_[merge_j_]->id()))
                merge_buffer_.push_back(tracks_[merge_i_++]);
            else
                merge_buffer_.push_back(tracks_[merge_j_++]);
            ++work;
            if (merge_i_ == merge_mid_ && merge_j_ == merge_right_) {
                merge_left_ = merge_right_;
                merge_pair_active_ = false;
            }
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
                return fail({CompileErrorCode::InvalidStructure, track->id(),
                             request_->document_revision});
            const auto clips = track->ordered_clip_ids();
            if (validation_clip_ < clips.size()) {
                const auto id = clips[validation_clip_++];
                ++work;
                if (!id.valid())
                    return fail({CompileErrorCode::InvalidStructure, id,
                                 request_->document_revision});
            } else {
                validation_clip_ = 0;
                ++validation_track_;
                ++work;
            }
            continue;
        }
        auto program = std::shared_ptr<const PlaybackProgram>(new PlaybackProgram(
            generation_, request_->document_revision, request_->project->id(),
            request_->sequence_id, request_->tempo_map, std::move(tracks_)));
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

PlaybackProgramCompiler::PlaybackProgramCompiler(
    PlaybackProgramStore& store, CompileExecutor& executor,
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
        (!request.dirty.all && request.dirty.tracks.empty() && request.track_policies.empty()))
        return reject({CompileErrorCode::InvalidRequest, {}, request.document_revision});
    const auto* sequence = request.project->find_sequence(request.sequence_id);
    if (!sequence)
        return reject({CompileErrorCode::InvalidRequest, request.sequence_id,
                       request.document_revision});
    std::sort(request.dirty.tracks.begin(), request.dirty.tracks.end());
    request.dirty.tracks.erase(std::unique(request.dirty.tracks.begin(), request.dirty.tracks.end()),
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
            return reject({CompileErrorCode::InvalidRequest, policy.track_id,
                           request.document_revision});
        request.dirty.tracks.push_back(policy.track_id);
    }
    std::sort(request.dirty.tracks.begin(), request.dirty.tracks.end());
    request.dirty.tracks.erase(std::unique(request.dirty.tracks.begin(), request.dirty.tracks.end()),
                               request.dirty.tracks.end());

    bool dispatch = false;
    {
        std::lock_guard lock(core_->mutex);
        if (!core_->accepting)
            return runtime::Err(core_->bound
                ? CompileError{CompileErrorCode::InvalidRequest, {}, request.document_revision}
                : CompileError{CompileErrorCode::CompilerAlreadyBound, {},
                               request.document_revision});
        if (request.document_revision <= core_->status.latest_submitted_revision) {
            ++core_->status.rejected_requests;
            const CompileError error{CompileErrorCode::StaleRevision, {}, request.document_revision};
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
                core_->pending->tempo_map.get() != request.tempo_map.get();
            request.dirty.tracks = std::move(dirty);

            const bool same_policy_domain =
                core_->pending->project->id() == request.project->id() &&
                core_->pending->sequence_id == request.sequence_id;
            if (same_policy_domain) {
                // Policies are sparse deltas. Preserve pending deltas that have
                // not reached publication; newest delta for a track wins.
                auto policies = std::move(core_->pending->track_policies);
                for (const auto& incoming : request.track_policies) {
                    const auto found = std::lower_bound(
                        policies.begin(), policies.end(), incoming.track_id,
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
        return runtime::Err(CompileError{CompileErrorCode::ExecutorUnavailable, {},
                                         submitted_revision});
    return runtime::Ok(CompileTicket{submitted_revision});
}

CompilerStatus PlaybackProgramCompiler::status() const {
    std::lock_guard lock(core_->mutex);
    return core_->status;
}

} // namespace pulp::playback
