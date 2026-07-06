#pragma once

// pulp::design — adherence lint.
//
// The mechanical backstop for the design contract (design_manifest.hpp): given
// UI JS and a compiled DesignManifest, flag the places where the code breaks out
// of the design system instead of binding to it. Three checks, all high-signal:
//
//   - raw_color:     a hex color literal (#rrggbb / #rgb / #rrggbbaa) where a
//                    bound theme should be referenced by token. When the value
//                    is one the system defines, the finding names the token.
//   - unknown_token: a `var(--name)` reference whose token is not in the
//                    manifest — a hallucinated or renamed token that would
//                    silently fall back at runtime (the exact failure the
//                    binding prompt exists to prevent).
//   - raw_dimension: an `<n>px` literal whose value matches a dimension token
//                    (info-level: prefer the token).
//
// The scan is purely lexical — no JS parse. Line and block comments are ignored
// (a color mentioned in a comment is not a binding); string literals are scanned
// (that is where style values live). Known limitation: raw_color matches any
// `#` + 3/6/8 hex-digit run outside a comment, so an all-hex CSS id selector such
// as `querySelector('#fff')` is reported as a raw color. Such ids are vanishingly
// rare in generated UI and `#fff` is almost always a color, so the check favors
// catching real drift over that edge.

#include <pulp/design/design_manifest.hpp>

#include <string>
#include <vector>

namespace pulp::design {

enum class AdherenceSeverity { error, warning, info };

enum class AdherenceKind {
    raw_color,      // hex color literal where a token is expected
    unknown_token,  // var(--x) reference to a token not in the manifest
    raw_dimension,  // <n>px literal matching a dimension token's value
};

struct AdherenceFinding {
    AdherenceKind kind;
    AdherenceSeverity severity;
    int line = 0;    // 1-based
    int column = 0;  // 1-based byte column of the snippet start
    std::string snippet;  // the offending literal / reference
    std::string message;  // human-readable, names a suggested token when known
};

/// Human-readable kind slug ("raw-color", "unknown-token", "raw-dimension").
const char* adherence_kind_name(AdherenceKind kind);

/// Lint UI JS against a compiled design contract. Findings are returned in
/// source order (by line, then column). An empty result means the JS binds
/// cleanly to the contract.
std::vector<AdherenceFinding> lint_adherence(const std::string& js_source,
                                             const DesignManifest& manifest);

}  // namespace pulp::design
