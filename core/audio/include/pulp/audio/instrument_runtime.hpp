#pragma once

#include <pulp/audio/sample_pool.hpp>
#include <pulp/audio/sample_zone_map.hpp>

namespace pulp::audio {

struct InstrumentTriggerResult {
    bool valid = false;
    ZoneSelection zone{};
    SamplePoolResolution sample{};
    double playback_rate = 0.0;
    double host_sample_rate = 0.0;
    double pitch_playback_rate = 0.0;
};

class InstrumentRuntime {
public:
    // RT-safe when zone_map and sample_pool are immutable for the duration of
    // the call and their borrowed publication/storage lifetimes are externally
    // guaranteed. This currently resolves pool-backed zones only; direct
    // PublishedSampleView zones remain usable through ZoneSelector. Trigger
    // policy only: voice allocation, choke groups, modulation buffers,
    // streaming, and rendering remain owned by their dedicated runtime paths.
    static InstrumentTriggerResult trigger(const SampleZoneMap& zone_map,
                                           const SamplePool& sample_pool,
                                           const ZoneSelectionRequest& request) noexcept;
};

}  // namespace pulp::audio
