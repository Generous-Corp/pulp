// SPDX-License-Identifier: MIT
//
// pulp #1031 — three-layer versioned detection for `pulp import-design`.
//
// Detects the (source, format-version, parser-version) tuple of an
// external design export by walking a file or directory and matching
// the fingerprint kinds described in `compat.json[imports/<source>/
// detected-formats]`.
//
// This module is intentionally self-contained: no pulp::view / pulp::state
// link deps so the detector can be unit-tested without dragging the full
// design-import pipeline (Skia, V8, Yoga, …) into the test binary.
//
// See `docs/reference/imports/index.md` for the recognized
// (source, format-version, parser-version) matrix and the fingerprint
// vocabulary. The schema is locked at `compat-schema-version: "0.2"`
// (#1031) — version bumps go through #1027's compat-sync hook.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulp::import_detect {

// One fingerprint clause from compat.json[imports/<src>/detected-formats/<i>/fingerprint].
struct FingerprintClause {
    enum class Kind {
        directory_files,        // {"kind":"directory-files", "files":[...]}
        html_script_src,        // {"kind":"html-script-src", "regex":"..."}
        html_script_type,       // {"kind":"html-script-type", "value":"..."}
        tailwind_config_token,  // {"kind":"tailwind-config-token", "any-of":[...]}
        filename,               // {"kind":"filename", "regex":"(?i)^DESIGN\\.md$"}
        frontmatter_fence,      // {"kind":"frontmatter-fence", "value":"---"}
        frontmatter_key,        // {"kind":"frontmatter-key", "required":"name"}
                                //   or {"kind":"frontmatter-key", "any-of":["colors","typography"]}
        json_key_equals,        // {"kind":"json-key-equals", "path":"provenance.adapter", "value":"figma-plugin"}
        unknown,
    };

    Kind kind = Kind::unknown;
    std::vector<std::string> files;     // directory-files
    std::string regex;                  // html-script-src, filename
    std::string value;                  // html-script-type, frontmatter-fence, json-key-equals
    std::vector<std::string> any_of;    // tailwind-config-token, frontmatter-key
    std::string required;               // frontmatter-key (single required key)
    std::string path;                   // json-key-equals — dotted path into the parsed JSON
    std::string raw_kind;               // for diagnostics on unknown clauses
};

// One format entry from compat.json[imports/<src>/detected-formats/<i>].
struct FormatEntry {
    std::string format_version;
    std::string parser_version;        // duplicated from the source-level
                                       // entry for convenience at match time
    std::string introduced;
    std::string deprecated;            // empty when null/absent
    std::vector<FingerprintClause> fingerprint;
    std::string notes;
    // Detection-strictness controls (used by DESIGN.md to avoid false
    // positives on generic Jekyll/Hugo Markdown frontmatter):
    //   match == "all-of"       → require matched == total
    //   min_confidence_pct > 0  → require confidence >= threshold
    std::string match;                 // empty or "all-of"
    int min_confidence_pct = 0;
};

// One source from compat.json[imports/<src>].
struct SourceEntry {
    std::string source;                // "stitch", "claude", ...
    std::string parser_version;
    std::vector<FormatEntry> formats;
};

// Top-level imports section parsed from compat.json.
struct ImportsManifest {
    std::string compat_schema_version;
    std::vector<SourceEntry> sources;
};

// Result of scanning an input file or directory and matching against
// every (source, format) pair in the manifest.
struct DetectionResult {
    bool ok = false;                   // false on schema/manifest errors
    std::string error;                 // populated when ok=false

    std::string source;                // empty when no match
    std::string format_version;
    std::string parser_version;
    int matched_clauses = 0;
    int total_clauses = 0;
    int confidence_pct = 0;            // 100 * matched / total
    std::vector<std::string> matched_kinds;     // distinct kind strings
    std::vector<std::string> unmatched_kinds;
};

// ── Manifest parsing ───────────────────────────────────────────────────

// Parse compat.json text → ImportsManifest. Returns std::nullopt on
// malformed JSON or missing `imports` section. Tolerates extra
// top-level keys so #1027's broader sections coexist with #1031's
// imports section.
std::optional<ImportsManifest> parse_compat_json(const std::string& text);

// Walk parents of `start_dir` (including `start_dir` itself) until a
// `compat.json` is found. Returns empty path when none.
std::filesystem::path find_compat_json(const std::filesystem::path& start_dir);

// ── Fingerprinting ─────────────────────────────────────────────────────

// Snapshot of the input file/directory used by every clause matcher.
// We compute it once per detect-only call; clauses are cheap arithmetic
// over the snapshot.
struct InputSnapshot {
    bool is_directory = false;
    std::filesystem::path root;
    std::string filename;                          // basename of the input file
                                                   // (empty when input is a directory)
    std::vector<std::string> directory_basenames;  // top-level only
    std::string html_text;             // primary HTML content (file or
                                       // dir/code.html / dir/index.html)
    std::vector<std::string> script_srcs;          // <script src="...">
    std::vector<std::string> script_types;         // <script type="...">
    std::vector<std::string> tailwind_tokens;      // tw.config.theme
                                                   // .extend.colors keys
                                                   // (and other config
                                                   // identifiers we grep for)
    // DESIGN.md import — populated when the input file ends in `.md`
    // and has a leading YAML frontmatter fence. The detector consumes
    // these via the frontmatter-fence and frontmatter-key clause kinds.
    bool has_frontmatter_fence = false;
    std::vector<std::string> frontmatter_keys;     // top-level YAML keys
};

// Build a snapshot from a file path or a directory path. Best-effort:
// missing pieces stay empty rather than failing.
InputSnapshot snapshot_input(const std::filesystem::path& input);

// Evaluate one clause against a snapshot.
bool match_clause(const FingerprintClause& clause, const InputSnapshot& snap);

// Detect the highest-confidence (source, format) for the given input.
// Iterates every entry in the manifest, scoring matched/total clauses.
// Returns the entry with the most matches; ties resolved in manifest
// order. When no entry has any matches, ok=true with empty source.
DetectionResult detect(const ImportsManifest& manifest,
                       const InputSnapshot& snap);

// ── Reporting ──────────────────────────────────────────────────────────

// Output of `--report-new-format`: a structured fingerprint diff
// suitable for hand-editing into a new `detected-formats` entry.
struct NewFormatReport {
    std::string candidate_source;
    std::string candidate_format_version;
    std::string based_on_source;
    std::string based_on_format_version;
    std::vector<std::string> additions;  // new tailwind tokens / scripts
    std::vector<std::string> removals;
};

// Build a NewFormatReport by diffing `snap` against the closest match
// in `manifest`. Suggests a candidate format-version derived from the
// closest match (e.g. "2025.04" → "2025.04+"). The candidate version
// is intentionally a placeholder — caller is expected to replace it
// with the real release date or upstream version string.
NewFormatReport build_new_format_report(const ImportsManifest& manifest,
                                        const InputSnapshot& snap,
                                        const DetectionResult& closest);

// Render the report as JSON suitable for piping into `compat.json`.
std::string render_new_format_json(const NewFormatReport& report);

}  // namespace pulp::import_detect
