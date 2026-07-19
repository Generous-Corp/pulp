#pragma once

/// @file design_tokens_w3c.hpp
/// W3C Design Tokens (DTCG) emitter for the design-import IR token set.
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
///   * string:    `{"$type": "string", "$value": "..."}`
/// A `source_identity` entry for the token's canonical path
/// ("colors." + name, etc.) is attached as
/// `{"$extensions": {"dev.pulp.source": {"id", "collection", "mode", "adapter"}}}`
/// with empty subfields omitted. Output key order is deterministic (sorted).
std::string to_w3c_tokens_json(const IRTokens& tokens, bool pretty = true);

}  // namespace pulp::view
