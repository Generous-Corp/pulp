#pragma once

#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_json.hpp>

#include <cstddef>
#include <vector>

namespace pulp::timeline::detail {

/// Decodes one enveloped `pulp.timeline.modulator` value.
///
/// No quota is charged here: the two callers charge from different counters --
/// a document from its track's running total, a command from the project
/// counts it is decoding into -- so the charge stays with the caller that knows
/// which budget the item belongs to.
runtime::Result<Modulator, PersistenceError> decode_modulator(const JsonValue& value,
                                                              std::string path);

/// Decodes one enveloped `pulp.timeline.macro_control` value. Charges no quota,
/// for the reason decode_modulator states.
runtime::Result<MacroControl, PersistenceError> decode_macro_control(const JsonValue& value,
                                                                     std::string path);

/// Decodes a track's `modulators` array, charging `count` against its quota.
runtime::Result<std::vector<Modulator>, PersistenceError>
decode_modulators(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                  std::string path);

/// Decodes a track's `macros` array, charging `count` against its quota.
runtime::Result<std::vector<MacroControl>, PersistenceError>
decode_macro_controls(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                      std::string path);

/// Decodes one enveloped `pulp.timeline.modulation_route` value.
///
/// Field shapes and the two enumerated spellings are checked here. Whether a
/// route's source and target exist on the owning track is a model invariant, so
/// it is enforced once by Track::create rather than a second time here. Charges
/// no quota, for the reason decode_modulator states.
runtime::Result<ModulationRoute, PersistenceError> decode_modulation_route(const JsonValue& value,
                                                                           std::string path);

/// Decodes a track's `modulation_routes` array, charging `count` against its quota.
runtime::Result<std::vector<ModulationRoute>, PersistenceError>
decode_modulation_routes(const JsonValue& value, const DecodeLimits& limits, std::size_t& count,
                         std::string path);

} // namespace pulp::timeline::detail
