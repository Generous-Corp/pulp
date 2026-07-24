#pragma once

#include <pulp/timeline/schema_json.hpp>

namespace pulp::timeline::detail {

runtime::Result<AudioLoopInfo, PersistenceError>
decode_audio_loop_info(const JsonValue& value, const DecodeLimits& limits,
                       std::size_t& point_count, std::size_t& tag_count, std::string path);

} // namespace pulp::timeline::detail
