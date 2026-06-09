#include <pulp/format/offline_sample_slot_bounce.hpp>

#include <cstdint>

namespace pulp::format {
namespace {

bool bank_prepared(const audio::SampleSlotBank& bank) noexcept {
    return bank.slot_count() > 0 && bank.max_channels() > 0 && bank.max_frames_per_slot() > 0;
}

} // namespace

OfflineSampleSlotBounceResult OfflineSampleSlotBounce::render_to_sample_slot(
    OfflineRenderHost& host,
    audio::SampleSlotBank& bank,
    const OfflineSampleBounceRequest& request) {
    OfflineSampleSlotBounceResult result;
    if (!bank_prepared(bank)) {
        result.bounce.status = OfflineSampleBounceStatus::bank_unprepared;
        return result;
    }

    result.bounce = OfflineSampleBounce::render_to_buffer(host, request);
    if (!result.bounce.ok()) return result;

    if (!bank.publish_from_buffer(result.bounce.render.audio.view(),
                                  static_cast<std::uint64_t>(
                                      result.bounce.render.audio.num_samples()),
                                  result.bounce.sample_rate,
                                  request.audio_safe_generation)) {
        result.bounce.status = OfflineSampleBounceStatus::publish_failed;
        return result;
    }

    result.published_view = bank.read_published_view();
    if (!result.published_view.valid) {
        result.bounce.status = OfflineSampleBounceStatus::publish_failed;
        return result;
    }

    result.bounce.status = OfflineSampleBounceStatus::ok;
    return result;
}

} // namespace pulp::format
