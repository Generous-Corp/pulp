#pragma once

// pulp::design — project design ledger.
//
// A DesignLedger is the small, CLI-maintained record of what a design/import
// project has produced and where it stands: each emitted artifact as a named,
// versioned asset with its source provenance, viewport, bound design system(s),
// and a review status. It is the resumability + audit substrate for multi-day,
// multi-agent design sessions — the answer to "which of these five ui.js
// revisions did the human approve" lives in the ledger, not in chat.
//
// Discipline (borrowed intentionally): only the CLI writes the ledger; skills
// and agents READ it (on resume, load the bound system's binding prompt without
// re-asking; never silently regenerate an approved version). Keeping writes on
// one path is what keeps the record trustworthy.
//
// This module is pure: parsing, serialization, and the in-memory operations
// carry no file IO. The CLI supplies the bytes and, for reconciliation, a
// predicate that answers whether an asset's file still exists on disk — so the
// core stays unit-testable and the "ledger vs reality" policy lives in one place.
//
// The serialized form is deterministic: assets are sorted by (name, version)
// and emitted in a fixed key order, so the ledger diffs cleanly in review.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::design {

/// On-disk format id stamped into DesignLedger::ledger_version; bump on a
/// non-additive shape change.
inline constexpr std::string_view kDesignLedgerVersion = "2026.07-design-ledger-v1";

/// Where an asset stands in review. A closed set — no free-text status — so a
/// consumer can branch on it reliably (e.g. never regenerate an `approved`
/// version).
enum class ReviewStatus { needs_review, approved, changes_requested };

/// Canonical lowercase slug for a status ("needs-review" | "approved" |
/// "changes-requested"). Stable across releases; used in JSON and CLI output.
const char* review_status_name(ReviewStatus status);

/// Parse a status slug; std::nullopt if it is not one of the three.
std::optional<ReviewStatus> review_status_from_name(std::string_view name);

/// One recorded artifact revision. Uniquely identified within a ledger by
/// (name, version).
struct LedgerAsset {
    std::string name;     ///< logical asset name (e.g. "main-panel")
    std::string version;  ///< monotonic per name ("v1", "v2", …); assigned by upsert if empty
    std::string path;     ///< emitted artifact path (e.g. "ui.js")
    std::string inherit_from;  ///< parent version this revision derives from ("" = root)
    std::string source;        ///< provenance ("fig", "figma-plugin", "claude", …)
    std::string viewport;      ///< design viewport ("340x280"), "" if unknown
    std::vector<std::string> design_systems;  ///< bound design system(s) ("ink-signal", …), sorted
    ReviewStatus status = ReviewStatus::needs_review;
};

/// The whole project ledger: a format id plus the recorded assets.
struct DesignLedger {
    std::string ledger_version;       ///< kDesignLedgerVersion when compiled here
    std::vector<LedgerAsset> assets;  ///< sorted by (name, version) for clean diffs
};

/// Parse a ledger from JSON. Tolerant: empty input or a shape it does not
/// recognize yields an empty ledger (so a first `record` bootstraps cleanly)
/// rather than throwing. Unknown status slugs fall back to needs_review.
DesignLedger parse_ledger(const std::string& json);

/// Serialize a ledger to deterministic JSON (assets sorted, fixed key order,
/// design_systems sorted). Stamps kDesignLedgerVersion.
std::string ledger_to_json(const DesignLedger& ledger);

/// Record or update an asset. If `incoming.version` is empty, the next version
/// for that name is assigned ("v1", "v2", …); if it names an existing
/// (name, version) the entry is replaced. `design_systems` is sorted+deduped.
/// Returns a reference to the stored entry. The ledger stays sorted afterward.
LedgerAsset& upsert_asset(DesignLedger& ledger, const LedgerAsset& incoming);

/// Remove a specific asset. `name` alone drops every version of that name;
/// "name@version" drops just that revision. Returns the slugs actually removed.
std::vector<std::string> remove_asset(DesignLedger& ledger, const std::string& selector);

/// Drop assets whose file no longer exists, per the caller-supplied predicate
/// (the "ledger vs reality" reconciler for hand-deleted files). Returns the
/// "name@version" slugs removed.
std::vector<std::string> reconcile(DesignLedger& ledger,
                                   const std::function<bool(const std::string& path)>& exists);

}  // namespace pulp::design
