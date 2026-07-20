#pragma once

#include "serialize_decode_support.hpp"

#include <pulp/timeline/serialize.hpp>

namespace pulp::timeline::detail {

using TrackDecodeFn = runtime::Result<Track, PersistenceError> (*)(
    const std::shared_ptr<const ParsedJson>&, const JsonValue&, const SchemaRegistry&,
    const DecodeLimits&, DecodeCounts&, std::string);

runtime::Result<Sequence, PersistenceError>
decode_sequence(const std::shared_ptr<const ParsedJson>& document, const JsonValue& value,
                const SchemaRegistry& registry, const DecodeLimits& limits, DecodeCounts& counts,
                std::string path, TrackDecodeFn decode_track);

} // namespace pulp::timeline::detail
