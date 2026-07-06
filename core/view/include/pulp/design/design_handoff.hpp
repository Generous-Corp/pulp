#pragma once

// pulp::design — Claude Design project-folder handoff contract.
//
// A design tool can export more than a single screen of HTML: a whole project
// folder carrying a design system (`_ds/<slug>/` — token CSS + a readme), a
// per-asset ledger (`.pulp-design-meta.json`, the same sidecar the CLI records),
// and a **handoff README** that states implementer intent in machine-readable
// prose — most importantly the explicit *fidelity declaration*: recreate the
// screens pixel-for-pixel (hi-fi) or restyle them into the target design system
// (lo-fi). That one bit prevents a whole class of wasted pixel-matching effort on
// designs that were only ever meant to be re-skinned.
//
// This translation unit parses that folder + README into a structured
// HandoffContract so the intent is a value the importer can act on instead of a
// paragraph a human has to read. It is pure: it takes already-read text and a
// directory listing, and does no filesystem or network I/O itself — the CLI owns
// the walk. The formats are community conventions, not a vendor API, so the
// contract is a versioned fingerprint (kHandoffFormatVersion) and every parse is
// tolerant: an absent section is empty, never an error.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::design {

/// Format id stamped into the contract; bump on a non-additive shape change.
inline constexpr std::string_view kHandoffFormatVersion = "2026.07-design-handoff-v1";

/// The implementer intent the handoff declares for the screens.
enum class FidelityIntent {
    unspecified,  ///< no declaration found — the importer must ask or default
    hifi,         ///< recreate pixel-for-pixel (the design IS the spec)
    lofi,         ///< restyle into the target design system (adapt, don't trace)
};

/// Stable lowercase slug for a fidelity intent ("hifi" | "lofi" | "unspecified").
std::string_view fidelity_intent_name(FidelityIntent intent);

/// Parse a fidelity value as written in a handoff README ("hi-fi", "high",
/// "pixel-perfect", "lo-fi", "restyle", …). Returns unspecified for anything
/// unrecognized so a typo never silently becomes the wrong intent.
FidelityIntent fidelity_intent_from_text(std::string_view text);

/// One screen the handoff describes: a display name, an optional relative path
/// to its source (from a `(path)` after the name), and its exact-value specs
/// (each a key/value bullet under the screen's heading).
struct HandoffScreen {
    std::string name;
    std::string path;  ///< empty when the heading carried no `(path)`
    std::vector<std::pair<std::string, std::string>> specs;
};

/// The parsed handoff contract.
struct HandoffContract {
    std::string format_version;                                  ///< kHandoffFormatVersion
    std::string source;                                          ///< folder or README path
    FidelityIntent fidelity = FidelityIntent::unspecified;
    std::vector<std::string> design_systems;                     ///< bound system slugs, sorted+unique
    std::vector<HandoffScreen> screens;                          ///< in document order
    std::vector<std::pair<std::string, std::string>> tokens;     ///< token table rows, in order
    std::vector<std::string> interactions;                       ///< interaction/state notes
};

/// Parse a handoff README (Markdown) into a contract. `source` is recorded
/// verbatim for provenance. Recognizes, case-insensitively and tolerantly:
///   - a `Fidelity:` line (or a `## Fidelity` heading) -> fidelity
///   - a `Bound to:`/`Design system:` line -> design_systems
///   - a `## Screens` section whose `###` headings are screens, with `- k: v`
///     bullets as specs and an optional `(relative/path)` after the name
///   - a `## Tokens` Markdown table (| Token | Value |) -> tokens
///   - a `## Interactions`/`## States` section's bullets -> interactions
HandoffContract parse_handoff_readme(std::string_view markdown, std::string_view source);

/// Merge design-system slugs discovered on disk (the `_ds/<slug>/` directory
/// names) into an already-parsed contract, keeping design_systems sorted+unique.
/// Lets the CLI fold folder structure into a README-derived contract.
void merge_design_systems(HandoffContract& contract, const std::vector<std::string>& slugs);

/// Serialize a contract to deterministic JSON for the import report.
std::string handoff_contract_json(const HandoffContract& contract);

}  // namespace pulp::design
