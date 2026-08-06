#pragma once

#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>

#include <cstddef>
#include <vector>

namespace pulp::timeline::detail {

/// Decodes a track's `modulators` array, charging `count` against its quota.
runtime::Result<std::vector<Modulator>, PersistenceError>
decode_modulators(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                  std::string path);

/// Decodes a track's `macros` array, charging `count` against its quota.
runtime::Result<std::vector<MacroControl>, PersistenceError>
decode_macro_controls(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                      std::string path);

/// Decodes a track's `modulation_routes` array, charging `count` against its quota.
///
/// Field shapes and the two enumerated spellings are checked here. Whether a
/// route's source and target exist on the owning track is a model invariant, so
/// it is enforced once by Track::create rather than a second time here.
runtime::Result<std::vector<ModulationRoute>, PersistenceError>
decode_modulation_routes(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                         std::string path);

} // namespace pulp::timeline::detail
