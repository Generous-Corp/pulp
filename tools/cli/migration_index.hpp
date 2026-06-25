// migration_index.hpp — embedded migration-note catalogue.
//
// Embedded catalogue of per-release migration notes. The generated
// translation unit `migration_index.cpp` is produced at CMake configure
// time by `tools/scripts/build_migration_index.py`, which scans
// `docs/migrations/*.md`, parses TOML frontmatter, and emits a table
// of `MigrationEntry` records plus a `kMigrationIndex` span.
//
// At runtime `cmd_upgrade --notes` (and `--notes --json`) filter the
// table using the `applies_if` expression so users see only the notes
// relevant to the specific upgrade hop they are performing.
//
// Deliberately decoupled from `cli_common.hpp` so the unit tests can
// link this TU standalone — same pattern as `version_diag.hpp` and
// `update_check.hpp`.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli::migration {

// ── Schema ──────────────────────────────────────────────────────────────────

// One row per migration doc. Fields mirror the TOML frontmatter defined
// at `docs/migrations/README.md` (or inline comment at the top of each
// migration file). `body_markdown` is the post-frontmatter body with a
// single leading newline trimmed — preserved so `--notes` can print it
// verbatim without stripping formatting.
struct MigrationEntry {
    std::string_view version;        // "0.27.0"
    bool breaking = false;           // `breaking = true` in frontmatter
    std::string_view applies_if;     // expression string (may be empty)
    std::string_view summary;        // one-line summary
    std::string_view body_markdown;  // everything after the closing `---`
};

// The generated table. Defined in migration_index.cpp (autogen).
// Ordered ascending by parsed semver so consumers can early-exit.
extern const MigrationEntry* const kMigrationIndex;
extern const std::size_t kMigrationIndexSize;

// ── Expression Evaluator ────────────────────────────────────────────────────
//
// `applies_if` is a tiny Boolean expression language over the two
// variables `cli_version_from` and `cli_version_to`, with the six
// comparison operators `<`, `<=`, `>`, `>=`, `==`, `!=` against a
// literal semver. Expressions may be combined with `&&` and `||`;
// parentheses group. Unknown syntax fails closed (returns false).
//
// Grammar (informal):
//
//     expr    := or
//     or      := and ("||" and)*
//     and     := cmp ("&&" cmp)*
//     cmp     := ident op version
//              | "(" expr ")"
//     ident   := "cli_version_from" | "cli_version_to"
//     op      := "<" | "<=" | ">" | ">=" | "==" | "!="
//     version := M "." N "." P   (with optional leading 'v')
//
// An empty expression matches every hop (used for non-breaking notes
// that are informative but don't gate on versions).

struct EvalContext {
    std::string version_from;  // the "from" side of the hop (installed)
    std::string version_to;    // the "to" side of the hop (target)
};

bool evaluate_applies_if(const std::string& expr, const EvalContext& ctx);

// ── Public lookup helpers ───────────────────────────────────────────────────

// Return the list of entries whose `version` is strictly newer than
// `from` and <= `to`, in ascending order. This models "notes that
// matter for the hop from X to Y" — the from-exclusive bound keeps
// `pulp upgrade --notes --from 0.27.0 --to 0.28.0` from re-printing
// 0.27.0 notes when stepping up from 0.27.0.
std::vector<const MigrationEntry*> entries_for_hop(const std::string& from,
                                                   const std::string& to);

// Same as above, then filters each entry through its `applies_if`
// expression. If an entry has an empty `applies_if`, it passes.
std::vector<const MigrationEntry*> applicable_entries(const std::string& from,
                                                      const std::string& to);

// Render the matching entries as human-readable text. Deterministic
// layout so shell-out tests can assert exact substrings.
std::string render_notes_text(const std::vector<const MigrationEntry*>& entries,
                              const std::string& from,
                              const std::string& to);

// Render as JSON. Stable-shape output for agent consumption: the
// `/upgrade` Claude Code skill depends on the keys listed here. Do NOT
// rename them without bumping the skill.
//
//   {
//     "from": "0.27.0",
//     "to":   "0.29.0",
//     "entries": [
//       {
//         "version":   "0.28.0",
//         "breaking":  true,
//         "summary":   "...",
//         "applies_if":"...",
//         "body":      "..."
//       },
//       ...
//     ]
//   }
std::string render_notes_json(const std::vector<const MigrationEntry*>& entries,
                              const std::string& from,
                              const std::string& to);

}  // namespace pulp::cli::migration
