#pragma once

/// @file design_tokens_w3c.hpp
/// W3C Design Tokens (DTCG) emitter + conformance validator for the
/// design-import IR token set.
///
/// Serializes `IRTokens` — the importer's flat name→value maps — into a
/// DTCG-conformant document. Distinct from `w3c_tokens.hpp`, which converts a
/// runtime `Theme` and stays always-compiled: this authoring-side emitter
/// depends on `design_ir.hpp` and lives in the `PULP_ENABLE_DESIGN_IMPORT`
/// cluster. It preserves the source tool's full token names ("/" group
/// separators nest into DTCG groups), emits the DTCG dimension object form
/// (`{"value": N, "unit": "px"}`), and carries `IRTokens::source_identity`
/// provenance under `$extensions["dev.pulp.source"]`.

#include <string>
#include <vector>

#include <pulp/view/design_ir.hpp>

namespace pulp::view {

/// Serialize design-import tokens to a W3C Design Tokens (DTCG) JSON document.
///
/// Top-level groups are `colors`, `dimensions`, and `strings`; an empty
/// category is omitted, and empty `IRTokens` yield `{}`. Within a group,
/// token names split on "/" into nested DTCG groups ("brand/primary" →
/// `{"brand": {"primary": {...}}}`). Leaf shapes:
///   * color:     `{"$type": "color", "$value": "#rrggbb"}`
///   * dimension: `{"$type": "dimension", "$value": {"value": N, "unit": "px"}}`
///
/// `IRTokens::strings` have no standard DTCG type, so no `$type` is invented:
///   * Names that clearly denote a font family (a "/"- or "."-segment equal to
///     `font`/`fontFamily`/`font-family`/`typeface`, or a final segment of
///     `family`/`font`, case-insensitive) are promoted to
///     `{"$type": "fontFamily", "$value": ...}` inside the `strings` group.
///     A comma-separated value ("Inter, sans-serif") becomes the DTCG array
///     form `["Inter", "sans-serif"]` (entries trimmed); otherwise the plain
///     string.
///   * Every other string is PARKED — never dropped — under the document-root
///     `$extensions["dev.pulp.nonStandardTokens"]` as
///     `{"<full name>": {"value": "<text>", "id", "collection", "mode",
///     "adapter"}}` with the provenance subfields from `source_identity`
///     (empties omitted). The real token groups therefore carry only
///     standard-typed tokens, and `validate_dtcg()` passes on emitted output.
///
/// A `source_identity` entry for the token's canonical path
/// ("colors." + name, etc.) is attached as
/// `{"$extensions": {"dev.pulp.source": {"id", "collection", "mode", "adapter"}}}`
/// with empty subfields omitted. Output key order is deterministic (sorted).
std::string to_w3c_tokens_json(const IRTokens& tokens, bool pretty = true);

/// Validate a JSON document against the DTCG invariants the emitter targets.
///
/// Returns human-readable violations; an empty vector means conformant.
/// Dependency-free (choc::json only) and best-effort by design — it checks:
///   * every leaf token (object with `$value`) has a resolvable `$type`
///     (own or inherited from an ancestor group) drawn from the standard
///     DTCG set (color, dimension, fontFamily, fontWeight, duration,
///     cubicBezier, number, strokeStyle, border, transition, shadow,
///     gradient, typography);
///   * `$value` shape matches `$type` where checkable: color → string,
///     dimension → `{value: number, unit: string}`, fontFamily → string or
///     array of strings;
///   * only the reserved keys `$value`/`$type`/`$description`/`$extensions`/
///     `$deprecated` appear; any other `$`-prefixed member is a violation;
///   * `$extensions` is an object whose keys are namespaced (contain ".");
///   * group members are objects (tokens or groups), and non-JSON input or a
///     non-object root is reported rather than thrown.
std::vector<std::string> validate_dtcg(const std::string& json);

}  // namespace pulp::view
