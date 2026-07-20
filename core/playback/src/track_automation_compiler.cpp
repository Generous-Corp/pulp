#include "track_automation_compiler.hpp"
#include "track_automation_order.hpp"

#include <pulp/playback/automation_program.hpp>

#include <utility>

namespace pulp::playback::detail {

void TrackAutomationCompiler::reset(
    const timeline::Track& track,
    std::shared_ptr<const timebase::CompiledTempoMap> tempo_map,
    ProgramGeneration generation, AutomationPlaybackLimits limits) {
    track_ = &track;
    tempo_map_ = std::move(tempo_map);
    generation_ = generation;
    limits_ = limits;
    stage_ = Stage::ValidateLimits;
    index_ = 0;
    point_count_ = 0;
    compiled_ = {};
    lane_programs_.clear();
    lane_compiler_active_ = false;
    lane_merge_buffer_.clear();
    programs_by_target_.clear();
    target_merge_buffer_.clear();
}

runtime::Result<TrackAutomationCompileStatus, TrackAutomationCompileError>
TrackAutomationCompiler::step() {
    if (stage_ == Stage::ValidateLimits) {
        if (track_->device_chain().size() > limits_.max_device_placements_per_track ||
            track_->automation_lanes().size() > limits_.max_lanes_per_track)
            return runtime::Err(TrackAutomationCompileError{track_->id()});
        stage_ = Stage::PreflightLanes;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::PreflightLanes) {
        if (index_ < track_->automation_lanes().size()) {
            const auto& lane = track_->automation_lanes()[index_++];
            const auto points = lane.curve().points().size();
            if (points > limits_.max_points_per_lane ||
                points > limits_.max_points_per_track - point_count_)
                return runtime::Err(TrackAutomationCompileError{lane.id()});
            point_count_ += points;
            return runtime::Ok(TrackAutomationCompileStatus::Pending);
        }
        index_ = 0;
        stage_ = Stage::PrepareStorage;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::PrepareStorage) {
        compiled_.ordered_device_placement_ids.reserve(track_->device_chain().size());
        lane_programs_.reserve(track_->automation_lanes().size());
        lane_merge_buffer_.reserve(track_->automation_lanes().size());
        programs_by_target_.reserve(track_->automation_lanes().size());
        target_merge_buffer_.reserve(track_->automation_lanes().size());
        stage_ = Stage::Placements;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::Placements) {
        if (index_ < track_->device_chain().size()) {
            compiled_.ordered_device_placement_ids.push_back(track_->device_chain()[index_++].id);
            return runtime::Ok(TrackAutomationCompileStatus::Pending);
        }
        index_ = 0;
        stage_ = Stage::Lanes;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::Lanes) {
        if (index_ < track_->automation_lanes().size()) {
            const auto& lane = track_->automation_lanes()[index_];
            if (!lane_compiler_active_) {
                lane_compiler_.reset(lane, tempo_map_, generation_);
                lane_compiler_active_ = true;
            }
            auto lane_step = lane_compiler_.step();
            if (!lane_step)
                return runtime::Err(TrackAutomationCompileError{lane_step.error().lane});
            if (lane_step.value() == AutomationProgramCompileStatus::Complete) {
                lane_programs_.push_back(lane_compiler_.take_result());
                lane_compiler_active_ = false;
                ++index_;
            }
            return runtime::Ok(TrackAutomationCompileStatus::Pending);
        }
        lane_merge_.reset(lane_merge_buffer_);
        stage_ = Stage::SortLanes;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::SortLanes) {
        const auto sorted =
            lane_merge_.step(lane_programs_, lane_merge_buffer_, automation_lane_less);
        if (sorted.complete) {
            index_ = 0;
            stage_ = Stage::ValidateLanes;
        }
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::ValidateLanes) {
        if (index_ < lane_programs_.size()) {
            const auto& program = lane_programs_[index_];
            const auto duplicate = index_ != 0 &&
                                   lane_programs_[index_ - 1]->lane_id() == program->lane_id();
            ++index_;
            if (!program->lane_id().valid() || program->lane_id() == track_->id() ||
                program->tempo_map_owner().get() != tempo_map_.get() || duplicate)
                return runtime::Err(TrackAutomationCompileError{program->lane_id()});
            return runtime::Ok(TrackAutomationCompileStatus::Pending);
        }
        index_ = 0;
        stage_ = Stage::PrepareTargets;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::PrepareTargets) {
        if (index_ < lane_programs_.size()) {
            programs_by_target_.push_back(lane_programs_[index_++].get());
            return runtime::Ok(TrackAutomationCompileStatus::Pending);
        }
        target_merge_.reset(target_merge_buffer_);
        stage_ = Stage::SortTargets;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::SortTargets) {
        const auto sorted =
            target_merge_.step(programs_by_target_, target_merge_buffer_, automation_target_less);
        if (sorted.complete) {
            index_ = 1;
            stage_ = Stage::ValidateTargets;
        }
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::ValidateTargets) {
        if (index_ < programs_by_target_.size()) {
            const auto* previous = programs_by_target_[index_ - 1];
            const auto* program = programs_by_target_[index_++];
            if (previous->target() == program->target())
                return runtime::Err(TrackAutomationCompileError{program->lane_id()});
            return runtime::Ok(TrackAutomationCompileStatus::Pending);
        }
        stage_ = Stage::Aggregate;
        return runtime::Ok(TrackAutomationCompileStatus::Pending);
    }
    if (stage_ == Stage::Aggregate) {
        compiled_.program = std::shared_ptr<const TrackAutomationProgram>(
            new TrackAutomationProgram(track_->id(), tempo_map_, std::move(lane_programs_)));
        stage_ = Stage::Complete;
        return runtime::Ok(TrackAutomationCompileStatus::Complete);
    }
    return runtime::Ok(TrackAutomationCompileStatus::Complete);
}

CompiledTrackAutomation TrackAutomationCompiler::take_result() noexcept {
    track_ = nullptr;
    tempo_map_.reset();
    generation_ = 0;
    index_ = 0;
    return std::move(compiled_);
}

} // namespace pulp::playback::detail
