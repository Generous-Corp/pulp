#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/audio/slice_map.hpp>

namespace pulp::audio {

inline constexpr std::uint32_t kNoSampleSliceIndex =
    static_cast<std::uint32_t>(-1);

struct SampleZone {
    PublishedSampleView sample{};

    int root_note = 60;
    int lowest_note = 0;
    int highest_note = 127;
    int lowest_velocity = 1;
    int highest_velocity = 127;

    // 100 cents per key gives chromatic playback from root_note.
    // 0 cents per key gives fixed-pitch trigger behavior.
    double keytrack_cents_per_key = 100.0;
    double tune_semitones = 0.0;

    // Higher priority wins. Equal-priority non-round-robin matches keep first
    // configured order; equal-priority round_robin_group matches rotate by the
    // request's round_robin_step.
    std::uint32_t priority = 0;
    std::uint32_t round_robin_group = 0;
    std::uint32_t voice_group = 0;
    std::uint32_t choke_group = 0;

    std::uint32_t slice_index = kNoSampleSliceIndex;
    SliceRegion slice_region{};

    bool has_loop = false;
    LoopRegion loop{};
};

struct ZoneSelectionRequest {
    int note = 60;
    int velocity = 100;
    std::uint32_t round_robin_step = 0;
    // 0.0 performs selection/pitch-only queries; playback_rate will be 0.0.
    double host_sample_rate = 0.0;
};

struct ZoneSelection {
    bool valid = false;
    std::uint32_t zone_index = 0;
    SampleZone zone{};
    double semitone_offset = 0.0;
    double pitch_ratio = 0.0;
    double playback_rate = 0.0;
};

class SampleZoneMap {
public:
    SampleZoneMap() = default;

    static bool zone_valid(const SampleZone& zone) noexcept;
    static bool request_valid(const ZoneSelectionRequest& request) noexcept;

    // Control/background-thread only. Copies and may allocate. Do not mutate a
    // map while an audio callback is selecting from it; publish whole-map
    // snapshots when sharing with RT readers.
    bool configure(std::span<const SampleZone> zones);
    void clear() noexcept;

    std::span<const SampleZone> zones() const noexcept { return zones_; }
    bool empty() const noexcept { return zones_.empty(); }
    std::size_t size() const noexcept { return zones_.size(); }

private:
    std::vector<SampleZone> zones_;
};

class ZoneSelector {
public:
    static bool matches(const SampleZone& zone,
                        const ZoneSelectionRequest& request) noexcept;

    static double semitone_offset_for_zone(const SampleZone& zone,
                                           int note) noexcept;
    static double pitch_ratio_for_zone(const SampleZone& zone,
                                       int note) noexcept;
    static double playback_rate_for_zone(const SampleZone& zone,
                                         int note,
                                         double host_sample_rate) noexcept;

    // RT-safe when the map is immutable for the duration of the call.
    static ZoneSelection select(const SampleZoneMap& map,
                                const ZoneSelectionRequest& request) noexcept;
};

}  // namespace pulp::audio
