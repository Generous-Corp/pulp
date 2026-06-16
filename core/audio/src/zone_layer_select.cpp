// zone_layer_select.cpp — multi-layer (multi-mic / velocity-layer) selection
// over a SampleZoneMap. See zone_layer_select.hpp for the contract.

#include <pulp/audio/zone_layer_select.hpp>

namespace pulp::audio {

std::size_t select_layers(const SampleZoneMap& map,
                          const ZoneSelectionRequest& request,
                          ZoneSelection* out, std::size_t max_out) noexcept {
    if (out == nullptr || max_out == 0) return 0;
    if (!SampleZoneMap::request_valid(request)) return 0;

    const auto zones = map.zones();
    std::size_t count = 0;

    for (std::uint32_t i = 0; i < zones.size() && count < max_out; ++i) {
        const auto& zone = zones[i];
        if (!ZoneSelector::matches(zone, request)) continue;

        // Round-robin variants (non-zero group) rotate: emit only the variant
        // selected by round_robin_step. Group-0 zones are independent layers and
        // all sound.
        if (zone.round_robin_group != 0) {
            std::uint32_t group_count = 0;
            std::uint32_t my_bucket = 0;
            bool found_self = false;
            for (std::uint32_t j = 0; j < zones.size(); ++j) {
                const auto& other = zones[j];
                if (other.round_robin_group != zone.round_robin_group) continue;
                if (!ZoneSelector::matches(other, request)) continue;
                if (j == i) {
                    my_bucket = group_count;
                    found_self = true;
                }
                ++group_count;
            }
            if (!found_self || group_count == 0) continue;
            if ((request.round_robin_step % group_count) != my_bucket) continue;
        }

        ZoneSelection sel;
        sel.valid = true;
        sel.zone_index = i;
        sel.zone = zone;
        sel.semitone_offset =
            ZoneSelector::semitone_offset_for_zone(zone, request.note);
        sel.pitch_ratio = ZoneSelector::pitch_ratio_for_zone(zone, request.note);
        sel.playback_rate = ZoneSelector::playback_rate_for_zone(
            zone, request.note, request.host_sample_rate);
        out[count++] = sel;
    }

    return count;
}

}  // namespace pulp::audio
