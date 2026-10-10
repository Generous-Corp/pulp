#pragma once

#include <pulp/audio/sample_asset_io.hpp>
#include <pulp/view/drag_drop.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulp::view {

struct SampleAssetDropPlan {
    std::vector<audio::SampleAssetDropItem> items;
    std::vector<std::string> probe_audio_paths;
    std::uint32_t accepted_count = 0;
    std::uint32_t rejected_count = 0;
    std::uint32_t audio_count = 0;
    std::uint32_t midi_count = 0;

    [[nodiscard]] bool accepted() const noexcept { return accepted_count > 0; }
    [[nodiscard]] bool needs_audio_probe() const noexcept { return !probe_audio_paths.empty(); }
};

[[nodiscard]] SampleAssetDropPlan classify_sample_asset_file_drop(
    const std::vector<std::string>& paths,
    const audio::SampleAssetPolicy& policy = {});

/// DropTarget adapter for sampler/import UIs. Drag enter/drop only perform
/// extension-level classification; expensive metadata probing is exposed as a
/// dispatch-request callback that must return quickly and hand work to a
/// background/control thread.
class SampleAssetDropTarget : public DropTarget {
public:
    // The plan reference is valid only for the callback duration. Async
    // dispatchers must copy paths/items they need after the call returns.
    using Callback = std::function<void(const SampleAssetDropPlan&, Point)>;

    SampleAssetDropTarget() = default;
    explicit SampleAssetDropTarget(audio::SampleAssetPolicy policy);

    void set_policy(audio::SampleAssetPolicy policy);
    const audio::SampleAssetPolicy& policy() const noexcept { return policy_; }

    const SampleAssetDropPlan& last_plan() const noexcept { return last_plan_; }

    Callback on_drag_classified;
    Callback on_drop_classified;
    Callback on_audio_probe_dispatch_requested;

    bool on_drag_enter(const DropData& data, Point position) override;
    void on_drag_exit() override;
    bool on_drop(const DropData& data, Point position) override;

private:
    audio::SampleAssetPolicy policy_;
    SampleAssetDropPlan last_plan_;
};

} // namespace pulp::view
