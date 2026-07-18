// SPDX-License-Identifier: MIT
//
// Classification of figma.com web-app URLs for `pulp import-design --url`.
//
// A figma.com file/design/proto link addresses the Figma web app, not the
// design data behind it. `--url` fetches unauthenticated (a bare `curl`, no
// credential flag exists), so such a link returns HTTP 403 for a private file
// and the single-page-app HTML shell for a public one — the latter then dies
// inside the JSON parser with an unrelated-looking error. Neither outcome is
// ever a successful import, so the CLI rejects these URLs up front and names
// the lanes that do work (desktop MCP, the "Design for Pulp" desktop plugin,
// `--from fig`, or figma_rest_export.py with a token).
//
// This module is intentionally self-contained — header-only, no pulp::view /
// pulp::state link deps — so the classification can be unit-tested without
// dragging the full design-import pipeline (Skia, V8, Yoga, …) into the test
// binary. Mirrors the layering rationale in `import_detect.hpp`.

#pragma once

#include <string>
#include <string_view>

namespace pulp::import_design {

/// True for a figma.com web-app scene URL (`/design/`, `/file/`, `/proto/`),
/// which `--url` can never import. Other figma.com paths are left alone: this
/// answers "is this the web app's scene route", not "is this Figma's domain".
inline bool is_figma_app_url(std::string_view url) {
    static constexpr std::string_view kPrefixes[] = {
        "https://figma.com/", "https://www.figma.com/",
        "http://figma.com/",  "http://www.figma.com/",
    };
    std::string_view rest;
    bool matched = false;
    for (const auto& prefix : kPrefixes) {
        if (url.size() > prefix.size() && url.compare(0, prefix.size(), prefix) == 0) {
            rest = url.substr(prefix.size());
            matched = true;
            break;
        }
    }
    if (!matched) return false;
    return rest.rfind("design/", 0) == 0 || rest.rfind("file/", 0) == 0 ||
           rest.rfind("proto/", 0) == 0;
}

/// The actionable error shown when a figma.com scene URL reaches `--url`.
/// Kept beside the classifier so the message and the rule stay in sync.
inline std::string figma_app_url_error() {
    return
        "Error: a figma.com file URL cannot be imported with --url.\n"
        "  --url fetches unauthenticated, and this CLI has no Figma credential,\n"
        "  so a Figma file URL returns a 403 or the web app's HTML shell.\n"
        "  Use one of these instead (local first):\n"
        "    1. Figma desktop MCP (get_design_context) to inspect the design.\n"
        "    2. The 'Design for Pulp' Figma desktop plugin, then:\n"
        "         pulp import-design --from figma-plugin --file <export>.pulp.zip\n"
        "    3. A local .fig save file:\n"
        "         pulp import-design --from fig --file design.fig --outline\n"
        "    4. Headless/CI, with a read-only token:\n"
        "         tools/import-design/figma_rest_export.py --token <pat> ...\n";
}

}  // namespace pulp::import_design
