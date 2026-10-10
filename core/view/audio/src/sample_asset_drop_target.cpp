#include <pulp/view/sample_asset_drop_target.hpp>

#include <utility>

namespace pulp::view {

SampleAssetDropPlan classify_sample_asset_file_drop(
    const std::vector<std::string>& paths,
    const audio::SampleAssetPolicy& policy) {
    SampleAssetDropPlan plan;
    plan.items = audio::classify_sample_asset_drop(paths, policy);
    plan.probe_audio_paths.reserve(plan.items.size());

    for (const auto& item : plan.items) {
        if (item.status == audio::SampleAssetStatus::ok) {
            ++plan.accepted_count;
            if (item.kind == audio::SampleAssetKind::audio) {
                ++plan.audio_count;
                plan.probe_audio_paths.push_back(item.path);
            } else if (item.kind == audio::SampleAssetKind::midi) {
                ++plan.midi_count;
            }
        } else {
            ++plan.rejected_count;
        }
    }

    return plan;
}

SampleAssetDropTarget::SampleAssetDropTarget(audio::SampleAssetPolicy policy)
    : policy_(std::move(policy)) {}

void SampleAssetDropTarget::set_policy(audio::SampleAssetPolicy policy) {
    policy_ = std::move(policy);
    last_plan_ = {};
}

bool SampleAssetDropTarget::on_drag_enter(const DropData& data, Point position) {
    if (data.type != DropData::Type::files) {
        last_plan_ = {};
        return false;
    }

    auto plan = classify_sample_asset_file_drop(data.file_paths, policy_);
    const bool accepted = plan.accepted();
    last_plan_ = plan;
    if (on_drag_classified) on_drag_classified(plan, position);
    return accepted;
}

void SampleAssetDropTarget::on_drag_exit() {
    last_plan_ = {};
}

bool SampleAssetDropTarget::on_drop(const DropData& data, Point position) {
    if (data.type != DropData::Type::files) {
        last_plan_ = {};
        return false;
    }

    auto plan = classify_sample_asset_file_drop(data.file_paths, policy_);
    const bool accepted = plan.accepted();
    const bool needs_audio_probe = plan.needs_audio_probe();
    last_plan_ = plan;

    if (on_drop_classified) on_drop_classified(plan, position);
    if (accepted && needs_audio_probe && on_audio_probe_dispatch_requested) {
        on_audio_probe_dispatch_requested(plan, position);
    }
    return accepted;
}

} // namespace pulp::view
