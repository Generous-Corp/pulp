// SPDX-License-Identifier: MIT
//
// Clean-room OUTPUT denylist scan for `pulp import emit`.
//
// The SDK writes the emission manifest's files; this scan is the safety net
// that a misbehaving importer cannot smuggle framework SOURCE or vendor banners
// into a `generated`/`stub` file. It is a real clean-room gate, not an
// exhaustive license scanner: it rejects the obvious framework-source tells
// (framework umbrella includes, build-system macros, copied-wrapper markers).
//
// `copied-user-file` provenance is EXEMPT — that is the user's own DSP, copied
// verbatim by the SDK and recorded in provenance; it is not generated output.
//
// Vendor-agnostic: this header and its .cpp name NO framework and NO vendor.
// The denylist tokens are runtime DATA. `denylist_from_known_frameworks()`
// sources them from the known-frameworks index's `content_match` markers — the
// ONE place real markers live — so the SDK code stays vendor-free while the
// gate still recognizes real framework-source tells.
#pragma once

#include "import_detect.hpp"
#include "import_emit.hpp"

#include <string>
#include <vector>

namespace pulp::cli::import_emit_scan {

// One denylist hit: which file, which token.
struct Hit {
    std::string path;   // the manifest file path that tripped the scan
    std::string token;  // the offending token
};

// Result of scanning a manifest's generated/stub files against a denylist.
struct ScanResult {
    bool clean = true;          // no hits
    std::vector<Hit> hits;      // every (file, token) that tripped
    int scanned_files = 0;      // generated/stub files examined
    int exempt_files = 0;       // copied-user-file entries skipped
};

// Build the output denylist from a known-frameworks index. Uses each
// framework's `content_match` markers (literal substrings) — those ARE the
// framework-source tells (umbrella includes, build macros, vendor namespaces).
// `file_glob` markers are path patterns, not content, so they are ignored.
std::vector<std::string> denylist_from_known_frameworks(
    const import_detect::KnownFrameworks& index);

// Scan every `generated`/`stub` file's inline content for any denylist token.
// `copied-user-file` entries are skipped (counted in exempt_files). Pure — no
// IO. Matching is case-insensitive substring (framework tells are distinctive
// enough that this gives no realistic false positives on hand-written Pulp).
ScanResult scan_manifest(const import_emit::Manifest& manifest,
                         const std::vector<std::string>& denylist);

}  // namespace pulp::cli::import_emit_scan
