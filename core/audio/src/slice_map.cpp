#include <pulp/audio/slice_map.hpp>

#include <cmath>

namespace pulp::audio {

bool validate_slice_map(const SliceMap& map) noexcept {
    if (map.source_frames == 0 || !(map.source_sample_rate > 0.0) ||
        !std::isfinite(map.source_sample_rate)) {
        return false;
    }
    if (map.markers.empty() || map.regions.empty()) return false;

    std::uint64_t previous_marker = 0;
    for (std::size_t i = 0; i < map.markers.size(); ++i) {
        const auto frame = map.markers[i].frame;
        if (frame >= map.source_frames) return false;
        if (i > 0 && frame <= previous_marker) return false;
        previous_marker = frame;
    }

    std::uint64_t previous_end = 0;
    for (const auto& region : map.regions) {
        if (region.start_frame >= region.end_frame ||
            region.end_frame > map.source_frames ||
            region.marker_index >= map.markers.size()) {
            return false;
        }
        if (region.start_frame < previous_end) return false;
        previous_end = region.end_frame;
    }
    return true;
}

}  // namespace pulp::audio

