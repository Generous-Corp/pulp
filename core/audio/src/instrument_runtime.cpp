#include <pulp/audio/instrument_runtime.hpp>

#include <cmath>

namespace pulp::audio {

namespace {

bool positive_finite(double value) noexcept {
    return value > 0.0 && std::isfinite(value);
}

double playback_rate_for_sample(const SamplePoolResolution& sample,
                                double pitch_ratio,
                                double host_sample_rate) noexcept {
    if (!sample.valid ||
        !positive_finite(sample.view.sample_rate) ||
        !positive_finite(host_sample_rate) ||
        pitch_ratio <= 0.0 ||
        !std::isfinite(pitch_ratio)) {
        return 0.0;
    }
    return pitch_ratio;
}

}  // namespace

InstrumentTriggerResult InstrumentRuntime::trigger(
    const SampleZoneMap& zone_map,
    const SamplePool& sample_pool,
    const ZoneSelectionRequest& request) noexcept {
    InstrumentTriggerResult result;

    const auto zone = ZoneSelector::select(zone_map, request);
    if (!zone.valid || zone.zone.sample_id == kInvalidSampleId) {
        return result;
    }

    const auto sample = sample_pool.resolve(zone.zone.sample_id);
    if (!sample.valid) return result;

    result.valid = true;
    result.zone = zone;
    result.sample = sample;
    result.playback_rate = playback_rate_for_sample(sample,
                                                    zone.pitch_ratio,
                                                    request.host_sample_rate);
    result.zone.playback_rate = result.playback_rate;
    return result;
}

}  // namespace pulp::audio
