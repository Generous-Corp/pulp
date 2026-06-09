#pragma once

#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/format/offline_sample_bounce.hpp>

namespace pulp::format {

struct OfflineSampleSlotBounceResult {
    OfflineSampleBounceResult bounce;
    audio::PublishedSampleView published_view{};

    [[nodiscard]] bool ok() const noexcept { return bounce.ok() && published_view.valid; }
    [[nodiscard]] OfflineSampleBounceStatus status() const noexcept { return bounce.status; }
};

/// Control/background-thread adapter from OfflineSampleBounce into
/// SampleSlotBank publication. This lives in the format layer because it needs
/// OfflineRenderHost; sampler playback and slot ownership remain in core/audio.
class OfflineSampleSlotBounce {
public:
    [[nodiscard]] static OfflineSampleSlotBounceResult render_to_sample_slot(
        OfflineRenderHost& host,
        audio::SampleSlotBank& bank,
        const OfflineSampleBounceRequest& request);
};

} // namespace pulp::format
