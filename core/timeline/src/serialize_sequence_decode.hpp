#pragma once

#include <pulp/timeline/serialize.hpp>

namespace pulp::timeline::detail {

struct DecodeCounts {
    std::size_t assets = 0;
    std::size_t sequences = 0;
    std::size_t tracks = 0;
    std::size_t clips = 0;
    std::size_t notes = 0;
    std::size_t device_placements = 0;
    std::size_t automation_lanes = 0;
    std::size_t automation_points = 0;
    std::size_t sequence_markers = 0;
    std::size_t sequence_regions = 0;
};

using TrackDecodeFn = runtime::Result<Track, PersistenceError> (*)(
    const std::shared_ptr<const ParsedJson>&, const JsonValue&, const SchemaRegistry&,
    const DecodeLimits&, DecodeCounts&, std::string);

runtime::Result<Sequence, PersistenceError>
decode_sequence(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
                const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
                std::string path, TrackDecodeFn decode_track);

} // namespace pulp::timeline::detail
