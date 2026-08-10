#pragma once

/// @file param_json.hpp
/// The one place a parameter becomes JSON.
///
/// A parameter's shape is about to have several consumers — the scripted-UI
/// bridge, the dev inspector, `@pulp/react`'s generated typings, the generated
/// JS-bridge reference, and later a UI lint that checks a script's `param="…"`
/// references against the declared set. Each of those could hand-roll its own
/// object, and then a field added for one silently would not exist for the
/// others.
///
/// So there is exactly one serializer, and every consumer calls it. Sync is not
/// a convention anyone has to remember; there is only one implementation to get
/// right. A second hand-rolled parameter payload appearing anywhere is a
/// review-stopper, not a style preference.
///
/// **Wire compatibility.** `param_snapshot_to_value` is what the inspector
/// protocol already sends. Its field names are load-bearing for existing
/// clients: `id`, `name`, `unit`, `value`, `normalized`, `modulated`, `default`,
/// `min`, `max`, and `display` (present only when non-empty). Those may not be
/// renamed or made unconditional. New fields are additive only, and
/// `test_param_json.cpp` pins the set so an accidental change fails a test
/// rather than a downstream client.
///
/// **Real units first.** Everything here reports a parameter's own domain, with
/// `normalized` alongside it, so a consumer never has to re-derive skew maths
/// from a 0-1 float. The bridge's older `getParam`/`setParam` remain normalized
/// and are deliberately untouched.

#include <string>

#include <choc/containers/choc_Value.h>

#include <pulp/state/parameter.hpp>

namespace pulp::state {

class StateStore;

/// Display text for `value` in `info`'s own domain.
///
/// Order: the author's `to_string` if they supplied one, else an enum label from
/// `value_labels` when the parameter is discrete, else a numeric fallback with
/// the unit appended. Callers get the same string the host shows, because they
/// are asking the same function.
std::string param_display_text(const ParamInfo& info, float value);

/// Parse display text back into `info`'s own domain.
///
/// Order: the author's `from_string` if supplied, else an enum label match, else
/// a numeric parse that tolerates a trailing unit ("440 Hz", "-3 dB", "50%").
/// The result is constrained to the declared range and step. Returns false when
/// the text names nothing — a caller must be able to tell "0" from "unparseable"
/// rather than silently storing a zero.
bool param_parse_display_text(const ParamInfo& info, const std::string& text, float& out_value);

/// A parameter's STATIC shape — everything that does not move when the value
/// does. Safe for a consumer to fetch once and cache.
///
/// `{ id, name, unit, min, max, default, step, skew, symmetricSkew, kind,
///    labels, groupId, designation, isTrigger }`
choc::value::Value param_metadata_to_value(const ParamInfo& info);

/// A parameter's LIVE snapshot, in the inspector's established wire shape.
///
/// `{ id, name, unit, value, normalized, modulated, default, min, max,
///    display? }` — `display` is omitted when empty, matching what the
/// inspector has always sent.
choc::value::Value param_snapshot_to_value(const StateStore& store, const ParamInfo& info);

/// The canonical Product A catalog + live-value shape. This is deliberately
/// assembled here so control adapters do not grow a second parameter JSON
/// serializer alongside scripted UI and the development tools.
///
/// `{ metadata fields..., value, normalized, modulated, display? }`
choc::value::Value param_catalog_snapshot_to_value(const StateStore& store, const ParamInfo& info);

} // namespace pulp::state
