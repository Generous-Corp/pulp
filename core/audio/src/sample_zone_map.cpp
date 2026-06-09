#include <pulp/audio/sample_zone_map.hpp>

#include <cmath>
#include <limits>

namespace pulp::audio {

namespace {

bool note_in_range(int note) noexcept {
    return note >= 0 && note <= 127;
}

bool velocity_in_range(int velocity) noexcept {
    return velocity >= 0 && velocity <= 127;
}

bool positive_finite(double value) noexcept {
    return value > 0.0 && std::isfinite(value);
}

bool valid_sample_view(const PublishedSampleView& sample) noexcept {
    return sample.valid &&
           sample.num_channels > 0 &&
           sample.num_frames > 0 &&
           positive_finite(sample.sample_rate);
}

bool valid_slice_region(const SliceRegion& region,
                        std::uint64_t source_frames) noexcept {
    return region.start_frame < region.end_frame &&
           region.end_frame <= source_frames;
}

bool same_round_robin_bucket(const SampleZone& zone,
                             const SampleZone& bucket) noexcept {
    return zone.priority == bucket.priority &&
           zone.round_robin_group != 0 &&
           zone.round_robin_group == bucket.round_robin_group;
}

bool matches_configured_zone(const SampleZone& zone,
                             const ZoneSelectionRequest& request) noexcept {
    return request.note >= zone.lowest_note &&
           request.note <= zone.highest_note &&
           request.velocity >= zone.lowest_velocity &&
           request.velocity <= zone.highest_velocity;
}

}  // namespace

bool SampleZoneMap::zone_valid(const SampleZone& zone) noexcept {
    const auto has_direct_sample = valid_sample_view(zone.sample);
    const auto has_sample_id = zone.sample_id != kInvalidSampleId;
    if (!has_direct_sample && !has_sample_id) return false;
    if (!note_in_range(zone.root_note) ||
        !note_in_range(zone.lowest_note) ||
        !note_in_range(zone.highest_note)) {
        return false;
    }
    if (!velocity_in_range(zone.lowest_velocity) ||
        !velocity_in_range(zone.highest_velocity)) {
        return false;
    }
    if (zone.lowest_note > zone.highest_note ||
        zone.lowest_velocity > zone.highest_velocity) {
        return false;
    }
    if (!std::isfinite(zone.keytrack_cents_per_key) ||
        !std::isfinite(zone.tune_semitones)) {
        return false;
    }
    // Pool-backed slice/loop validation needs a resolved pool sample's frame
    // count; keep that out of the zone-only validator for this slice.
    if (zone.slice_index != kNoSampleSliceIndex && !has_direct_sample) {
        return false;
    }
    if (zone.slice_index != kNoSampleSliceIndex &&
        !valid_slice_region(zone.slice_region, zone.sample.num_frames)) {
        return false;
    }
    if (zone.has_loop && !has_direct_sample) {
        return false;
    }
    if (zone.has_loop &&
        !validate_loop_region(zone.loop, zone.sample.num_frames).ok) {
        return false;
    }
    return true;
}

bool SampleZoneMap::request_valid(const ZoneSelectionRequest& request) noexcept {
    return note_in_range(request.note) &&
           velocity_in_range(request.velocity) &&
           request.velocity > 0 &&
           (request.host_sample_rate == 0.0 || positive_finite(request.host_sample_rate));
}

bool SampleZoneMap::configure(std::span<const SampleZone> zones) {
    if (zones.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    for (const auto& zone : zones) {
        if (!zone_valid(zone)) return false;
    }
    zones_.assign(zones.begin(), zones.end());
    return true;
}

void SampleZoneMap::clear() noexcept {
    zones_.clear();
}

bool ZoneSelector::matches(const SampleZone& zone,
                           const ZoneSelectionRequest& request) noexcept {
    if (!SampleZoneMap::zone_valid(zone) ||
        !SampleZoneMap::request_valid(request)) {
        return false;
    }
    return matches_configured_zone(zone, request);
}

double ZoneSelector::semitone_offset_for_zone(const SampleZone& zone,
                                              int note) noexcept {
    if (!SampleZoneMap::zone_valid(zone) || !note_in_range(note)) return 0.0;
    const auto key_delta = static_cast<double>(note - zone.root_note);
    return zone.tune_semitones +
           key_delta * (zone.keytrack_cents_per_key / 100.0);
}

double ZoneSelector::pitch_ratio_for_zone(const SampleZone& zone,
                                          int note) noexcept {
    if (!SampleZoneMap::zone_valid(zone) || !note_in_range(note)) return 0.0;
    return std::pow(2.0, semitone_offset_for_zone(zone, note) / 12.0);
}

double ZoneSelector::playback_rate_for_zone(const SampleZone& zone,
                                            int note,
                                            double host_sample_rate) noexcept {
    if (!positive_finite(host_sample_rate) ||
        !valid_sample_view(zone.sample) ||
        !positive_finite(zone.sample.sample_rate)) {
        return 0.0;
    }
    const auto ratio = pitch_ratio_for_zone(zone, note);
    if (ratio == 0.0) return 0.0;
    return ratio * (zone.sample.sample_rate / host_sample_rate);
}

ZoneSelection ZoneSelector::select(const SampleZoneMap& map,
                                   const ZoneSelectionRequest& request) noexcept {
    ZoneSelection result;
    if (!SampleZoneMap::request_valid(request)) return result;

    auto zones = map.zones();
    std::uint32_t best_index = 0;
    const SampleZone* best = nullptr;
    auto best_priority = std::numeric_limits<std::uint32_t>::min();

    for (std::uint32_t index = 0; index < zones.size(); ++index) {
        const auto& zone = zones[index];
        if (!matches_configured_zone(zone, request)) continue;
        if (!best || zone.priority > best_priority) {
            best = &zone;
            best_index = index;
            best_priority = zone.priority;
        }
    }

    if (!best) return result;

    if (best->round_robin_group != 0) {
        std::uint32_t matching_group_count = 0;
        for (const auto& zone : zones) {
            if (matches_configured_zone(zone, request) &&
                same_round_robin_bucket(zone, *best)) {
                ++matching_group_count;
            }
        }

        if (matching_group_count > 0) {
            const auto target = request.round_robin_step % matching_group_count;
            std::uint32_t bucket_index = 0;
            for (std::uint32_t index = 0; index < zones.size(); ++index) {
                const auto& zone = zones[index];
                if (!matches_configured_zone(zone, request) ||
                    !same_round_robin_bucket(zone, *best)) {
                    continue;
                }
                if (bucket_index == target) {
                    best = &zone;
                    best_index = index;
                    break;
                }
                ++bucket_index;
            }
        }
    }

    result.valid = true;
    result.zone_index = best_index;
    result.zone = *best;
    result.semitone_offset = semitone_offset_for_zone(*best, request.note);
    result.pitch_ratio = pitch_ratio_for_zone(*best, request.note);
    result.playback_rate = playback_rate_for_zone(*best,
                                                  request.note,
                                                  request.host_sample_rate);
    return result;
}

}  // namespace pulp::audio
