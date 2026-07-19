// test_design_import_parity.cpp — four-surface field-parity guard for the
// design-import codegen.
//
// Pulp lowers DesignIR through FOUR codegen surfaces:
//
//   js     core/view/src/design_codegen.cpp            (web-compat DOM + native-bridge JS)
//   cpp    core/view/src/design_cpp_codegen.cpp        (native C++ source codegen)
//   swift  core/view/src/design_swift_codegen.cpp      (SwiftUI codegen)
//   native core/view/src/design_import_native_common.cpp (live-native View materialization)
//
// A typed `IRStyle`/`IRLayout` field can be lowered in one surface and
// silently skipped in the others (real examples: `h_constraint`/`v_constraint`
// lower only in the JS lane; `mix_blend_mode` and `text_runs` skip cpp+native).
// This test makes that gap loud: every lowerable field must either be
// referenced by ALL FOUR surface files, or carry an explicit allowlist entry
// below naming exactly which surfaces skip it and why. Adding a new IR field
// to one lane without lowering it in the other three — or allowlisting the
// partial — is a test failure, not a silent divergence.
//
// The allowlist is the honest ledger of cross-surface partials (it mirrors the
// `codegen: partial` rows in compat.json). Entries are exact: when a field
// STARTS lowering in a previously-missing surface the entry goes stale and the
// test fails until the entry is trimmed — progress must be recorded, too.
//
// Mechanism: the field list is parsed at runtime from the single source of
// truth (`design_ir.hpp`'s IRStyle/IRLayout members), so a new field is picked
// up automatically. "Referenced" means a member access of the field appears in
// the surface file (`.field` or `->field`, whole identifier). That is a
// necessary-not-sufficient signal — a surface could read a field and lower it
// badly — but it is exactly the signal that catches the silent-skip failure
// mode this guard exists for.
//
// Tag: [view][import][parity]

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef PULP_SRC_DIR
#error "PULP_SRC_DIR must be defined (path to the repo root)"
#endif

namespace {

// ── The four codegen surfaces ───────────────────────────────────────────────

struct Surface {
    std::string key;   // short name used in allowlist entries + messages
    std::string path;  // repo-relative source path
};

const std::vector<Surface>& surfaces() {
    static const std::vector<Surface> s = {
        {"js", "core/view/src/design_codegen.cpp"},
        {"cpp", "core/view/src/design_cpp_codegen.cpp"},
        {"swift", "core/view/src/design_swift_codegen.cpp"},
        {"native", "core/view/src/design_import_native_common.cpp"},
    };
    return s;
}

// ── Allowlist: the honest ledger of cross-surface partials ──────────────────
//
// Each entry names the surfaces that are EXPECTED to skip the field, plus a
// one-line reason. The test fails when reality drifts in either direction:
//   * a listed surface now references the field  → trim the entry (progress)
//   * an unlisted surface stopped referencing it → regression (or new field
//     landed in one lane only)

struct AllowlistEntry {
    std::set<std::string> missing;  // surface keys expected to skip the field
    std::string reason;
};

const std::map<std::string, AllowlistEntry>& allowlist() {
    static const std::map<std::string, AllowlistEntry> a = {
        // ── IRStyle ────────────────────────────────────────────────────────
        {"background_image",
         {{"js", "cpp", "swift", "native"},
          "no codegen reads it directly; image fills become dedicated image "
          "nodes/assets at IR build time, raster background-image paint is the "
          "View::style_extras deferred slot"}},
        {"background_repeat",
         {{"swift"}, "js+cpp+native lower it; SwiftUI tiling deferred"}},
        {"background_size",
         {{"swift"}, "js+cpp+native lower it; SwiftUI background sizing deferred"}},
        {"object_fit",
         {{"swift"}, "js+cpp+native lower it; SwiftUI content-mode mapping deferred"}},
        {"mix_blend_mode",
         {{"cpp", "native"},
          "js+swift lower it; cpp codegen and native materializer skip it "
          "(known partial — View::set_mix_blend_mode exists but is not wired)"}},
        {"border",
         {{"cpp", "swift", "native"},
          "raw CSS shorthand passthrough emitted by the JS lane only; the "
          "other surfaces consume the parsed border_color/width/style fields"}},
        {"border_style",
         {{"cpp", "swift"},
          "js+native lower it; cpp/swift emit solid borders only"}},
        {"box_shadow",
         {{"cpp"}, "js+swift+native lower shadow layers; cpp codegen deferred"}},
        {"filter",
         {{"cpp", "swift"}, "js+native lower it; cpp/swift filter emission deferred"}},
        {"backdrop_filter",
         {{"cpp", "swift"}, "js+native lower it; cpp/swift deferred"}},
        {"clip_path",
         {{"cpp", "swift", "native"},
          "js-only; engine consumes it via the setClipPath bridge, other "
          "surfaces deferred"}},
        {"mask",
         {{"cpp", "swift", "native"}, "js-only; setMask bridge consumer, others deferred"}},
        {"mask_image",
         {{"cpp", "swift", "native"}, "js-only; setMaskImage bridge consumer, others deferred"}},
        {"mask_size",
         {{"cpp", "swift", "native"}, "js-only; setMaskSize bridge consumer, others deferred"}},
        {"font_family",
         {{"swift"}, "js+cpp+native lower it; SwiftUI uses system font stack"}},
        {"text_align",
         {{"swift"}, "js+cpp+native lower it; SwiftUI alignment mapping deferred"}},
        {"vertical_align",
         {{"cpp", "swift", "native"},
          "js-only; native-common derives vertical centering from a "
          "slot-vs-font heuristic instead of reading the IR field"}},
        {"letter_spacing",
         {{"swift"}, "js+cpp+native lower it; SwiftUI kerning deferred"}},
        {"line_height",
         {{"swift"}, "js+cpp+native lower it; SwiftUI line spacing deferred"}},
        {"text_transform",
         {{"swift"}, "js+cpp+native lower it; SwiftUI case transform deferred"}},
        {"white_space",
         {{"js", "swift"},
          "cpp+native lower it; the JS lane relies on web-compat CSS defaults "
          "and SwiftUI wrapping is deferred"}},
        {"text_overflow",
         {{"js", "cpp", "swift", "native"},
          "parsed into the IR but no codegen lowers it yet (ellipsis clipping "
          "deferred on every surface)"}},
        {"overflow",
         {{"swift"}, "js+cpp+native lower it; SwiftUI clipping/scroll deferred"}},
        {"cursor",
         {{"cpp", "swift", "native"},
          "web-only affordance; native surfaces have no pointer-cursor styling"}},
        {"right",
         {{"swift"},
          "js+cpp+native lower right-anchored absolute offsets; SwiftUI "
          "offsets from left/top only"}},
        {"z_index",
         {{"swift"},
          "js+cpp+native lower it; SwiftUI relies on declaration order"}},
        {"transform",
         {{"cpp"}, "js+swift+native lower it; cpp codegen transform deferred"}},
        {"min_width", {{"swift"}, "js+cpp+native lower it; SwiftUI min sizing deferred"}},
        {"min_height", {{"swift"}, "js+cpp+native lower it; SwiftUI min sizing deferred"}},
        {"max_width", {{"swift"}, "js+cpp+native lower it; SwiftUI max sizing deferred"}},
        {"max_height", {{"swift"}, "js+cpp+native lower it; SwiftUI max sizing deferred"}},
        {"render_bounds",
         {{"cpp", "swift"},
          "js+native honor figma-plugin render-bounds bleed; cpp/swift deferred"}},

        // ── IRLayout ───────────────────────────────────────────────────────
        {"margin_top",
         {{"js", "swift"},
          "cpp+native lower margins; the JS lane only emits auto-margins "
          "derived from constraints, and SwiftUI margins are deferred"}},
        {"margin_right", {{"js", "swift"}, "same as margin_top"}},
        {"margin_bottom", {{"js", "swift"}, "same as margin_top"}},
        {"margin_left", {{"js", "swift"}, "same as margin_top"}},
        {"align_self",
         {{"swift"}, "js+cpp+native lower it; SwiftUI per-item alignment deferred"}},
        {"align_content",
         {{"swift"}, "js+cpp+native lower it; SwiftUI wrapped-line alignment deferred"}},
        {"flex_grow",
         {{"swift"}, "js+cpp+native lower it; SwiftUI flexible sizing deferred"}},
        {"flex_shrink",
         {{"swift"}, "js+cpp+native lower it; SwiftUI flexible sizing deferred"}},
        {"flex_basis",
         {{"js", "swift"},
          "cpp+native lower it; JS lane and SwiftUI flex-basis deferred"}},
        {"order",
         {{"js", "swift"},
          "cpp+native lower it; JS lane and SwiftUI item reordering deferred"}},
        {"aspect_ratio",
         {{"swift"}, "js+cpp+native lower it; SwiftUI aspectRatio mapping deferred"}},
        {"overflow_x",
         {{"js", "cpp", "swift", "native"},
          "parsed into the IR but no codegen lowers per-axis overflow yet "
          "(the shorthand `overflow` style field is what surfaces consume)"}},
        {"overflow_y",
         {{"js", "cpp", "swift", "native"}, "same as overflow_x"}},
        {"width_mode",
         {{"swift"}, "js+cpp+native lower hug/fill sizing; SwiftUI deferred"}},
        {"height_mode",
         {{"swift"}, "js+cpp+native lower hug/fill sizing; SwiftUI deferred"}},
        {"h_constraint",
         {{"cpp", "swift", "native"},
          "js-only; figma resize constraints map onto flex auto-margins in the "
          "web lane, not modeled in cpp/swift/native-common yet"}},
        {"v_constraint",
         {{"cpp", "swift", "native"}, "same as h_constraint"}},
        {"grid_auto_flow",
         {{"cpp", "swift", "native"},
          "js-only; the other surfaces lower explicit grid templates/placement "
          "but not auto-flow yet"}},

        // ── IRNode extras (see checked_node_fields) ────────────────────────
        {"text_runs",
         {{"cpp", "native"},
          "js+swift lower per-range mixed-text styling; cpp codegen and the "
          "native materializer emit the plain text only"}},
    };
    return a;
}

// IRNode-level fields that are lowering obligations for every surface, checked
// alongside the parsed IRStyle/IRLayout members. Most IRNode fields are
// consumer-only metadata (provenance, anchors, the free-form `attributes`
// map — intentionally NOT parity-checked), so obligations are opted in here
// explicitly rather than parsed.
const std::vector<std::string>& checked_node_fields() {
    static const std::vector<std::string> f = {"text_runs"};
    return f;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    INFO("reading " << path);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Strip // and /* */ comments so commented-out code never counts as a
/// reference and doc comments never confuse the header parser.
std::string strip_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size();) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i = (i + 1 < src.size()) ? i + 2 : src.size();
            out += ' ';  // keep tokens separated
        } else {
            out += src[i++];
        }
    }
    return out;
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// Extract the body text of `struct <name> { ... };` (comment-stripped input).
std::string struct_body(const std::string& header, const std::string& name) {
    const std::string needle = "struct " + name;
    std::size_t pos = header.find(needle);
    INFO("locating 'struct " << name << "' in design_ir.hpp");
    REQUIRE(pos != std::string::npos);
    std::size_t open = header.find('{', pos + needle.size());
    REQUIRE(open != std::string::npos);
    int depth = 0;
    for (std::size_t i = open; i < header.size(); ++i) {
        if (header[i] == '{') ++depth;
        else if (header[i] == '}') {
            if (--depth == 0) return header.substr(open + 1, i - open - 1);
        }
    }
    FAIL("unbalanced braces parsing struct " << name);
    return {};
}

/// Parse the data-member names of a struct body. Handles `std::optional<T> a;`,
/// defaulted members (`float gap = 0.0f;`), and comma declarator lists
/// (`std::optional<float> top, left, right, bottom;`). Skips nested types,
/// using-declarations, and functions. This is deliberately a narrow parser for
/// design_ir.hpp's plain aggregate style — the sanity checks in the test body
/// fail loudly if the header outgrows it.
std::vector<std::string> parse_member_names(const std::string& body) {
    std::vector<std::string> names;
    std::string stmt;
    int depth = 0;      // brace depth (nested struct bodies are skipped)
    int angle = 0;      // template-argument depth
    for (char c : body) {
        if (c == '{') { ++depth; stmt += c; continue; }
        if (c == '}') { --depth; stmt += c; continue; }
        if (depth > 0) { stmt += c; continue; }
        if (c == '<') ++angle;
        else if (c == '>') --angle;
        if (c == ';' && angle == 0) {
            // One top-level statement collected.
            std::string s = stmt;
            stmt.clear();
            // Trim.
            std::size_t b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) continue;
            std::size_t e = s.find_last_not_of(" \t\r\n");
            s = s.substr(b, e - b + 1);
            if (s.rfind("struct", 0) == 0 || s.rfind("using", 0) == 0 ||
                s.rfind("enum", 0) == 0 || s.find('(') != std::string::npos)
                continue;  // nested type, alias, or function — not a data member
            // Split on top-level commas: "type first = x, second = y, third".
            std::vector<std::string> chunks;
            std::string cur;
            int a2 = 0;
            for (char d : s) {
                if (d == '<') ++a2;
                else if (d == '>') --a2;
                if (d == ',' && a2 == 0) { chunks.push_back(cur); cur.clear(); }
                else cur += d;
            }
            chunks.push_back(cur);
            for (std::size_t ci = 0; ci < chunks.size(); ++ci) {
                std::string chunk = chunks[ci];
                if (std::size_t eq = chunk.find('='); eq != std::string::npos)
                    chunk = chunk.substr(0, eq);  // drop initializer
                // First chunk is "type name" (take last identifier); later
                // chunks are bare declarators (also their last identifier).
                std::size_t end = chunk.find_last_not_of(" \t\r\n");
                if (end == std::string::npos) continue;
                std::size_t start = end;
                while (start > 0 && is_ident_char(chunk[start - 1])) --start;
                if (!is_ident_char(chunk[end])) continue;
                names.push_back(chunk.substr(start, end - start + 1));
            }
        } else {
            stmt += c;
        }
    }
    return names;
}

/// True when the surface source contains a member access of `field`
/// (`.field` or `->field` as a whole identifier). Reading the field is the
/// minimum evidence that the surface lowers (or deliberately consumes) it.
bool references_field(const std::string& src, const std::string& field) {
    for (std::size_t pos = src.find(field); pos != std::string::npos;
         pos = src.find(field, pos + 1)) {
        // Whole identifier: nothing word-like on either side.
        if (pos + field.size() < src.size() && is_ident_char(src[pos + field.size()]))
            continue;
        if (pos > 0 && is_ident_char(src[pos - 1])) continue;
        // Preceded (over whitespace) by '.' or '->'.
        std::size_t p = pos;
        while (p > 0 && (src[p - 1] == ' ' || src[p - 1] == '\t' ||
                         src[p - 1] == '\n' || src[p - 1] == '\r'))
            --p;
        if (p > 0 && (src[p - 1] == '.' || src[p - 1] == '>')) return true;
    }
    return false;
}

std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) out += (out.empty() ? "" : ", ") + v;
    return out;
}

}  // namespace

TEST_CASE("every lowerable IR field reaches all four codegen surfaces or is allowlisted",
          "[view][import][parity]") {
    const std::string root = std::string(PULP_SRC_DIR) + "/";

    // Field list: single source of truth is design_ir.hpp.
    const std::string header =
        strip_comments(read_file(root + "core/view/include/pulp/view/design_ir.hpp"));
    std::vector<std::string> fields;
    for (const auto& name : parse_member_names(struct_body(header, "IRStyle")))
        fields.push_back(name);
    const std::size_t style_count = fields.size();
    for (const auto& name : parse_member_names(struct_body(header, "IRLayout")))
        fields.push_back(name);
    const std::size_t layout_count = fields.size() - style_count;
    for (const auto& name : checked_node_fields()) fields.push_back(name);

    // Sanity: the narrow header parser must keep tracking design_ir.hpp. If
    // these fail, the parser rotted — fix parse_member_names, don't relax the
    // guard.
    {
        const std::set<std::string> parsed(fields.begin(), fields.end());
        for (const char* known :
             {"background_color", "mix_blend_mode", "vertical_align", "top",
              "bottom", "gap", "padding_left", "h_constraint", "grid_row"}) {
            INFO("parser sanity: expected member '" << known
                 << "' parsed from design_ir.hpp");
            REQUIRE(parsed.count(known) == 1);
        }
        INFO("parser sanity: IRStyle member count " << style_count
             << ", IRLayout member count " << layout_count);
        REQUIRE(style_count >= 40);
        REQUIRE(layout_count >= 25);
    }

    // Load the four surfaces (comment-stripped so commented-out lowering
    // doesn't count as parity).
    std::map<std::string, std::string> surface_src;
    for (const auto& s : surfaces()) {
        surface_src[s.key] = strip_comments(read_file(root + s.path));
        INFO("surface " << s.key << " (" << s.path << ") looks implausibly small");
        REQUIRE(surface_src[s.key].size() > 4096);
    }

    std::vector<std::string> failures;
    std::set<std::string> seen;  // guards duplicate declarator handling
    for (const auto& field : fields) {
        if (!seen.insert(field).second) continue;

        std::set<std::string> missing;
        for (const auto& s : surfaces())
            if (!references_field(surface_src[s.key], field)) missing.insert(s.key);

        const auto entry = allowlist().find(field);
        const std::set<std::string> expected =
            entry == allowlist().end() ? std::set<std::string>{} : entry->second.missing;

        if (missing == expected) continue;

        std::ostringstream msg;
        msg << "field '" << field << "': ";
        std::set<std::string> newly_missing, newly_present;
        for (const auto& k : missing)
            if (!expected.count(k)) newly_missing.insert(k);
        for (const auto& k : expected)
            if (!missing.count(k)) newly_present.insert(k);
        if (!newly_missing.empty()) {
            msg << "not lowered in [" << join(newly_missing) << "]";
            if (missing.size() < surfaces().size()) {
                std::set<std::string> present;
                for (const auto& s : surfaces())
                    if (!missing.count(s.key)) present.insert(s.key);
                msg << " while lowered in [" << join(present) << "]";
            }
            msg << " — lower it there or add/extend the allowlist entry in "
                   "test_design_import_parity.cpp documenting the partial.";
        }
        if (!newly_present.empty()) {
            if (!newly_missing.empty()) msg << " ALSO: ";
            msg << "now referenced in [" << join(newly_present)
                << "] but still allowlisted as missing — trim the allowlist "
                   "entry so the ledger records the progress.";
        }
        failures.push_back(msg.str());
    }

    // Stale entries for fields that no longer exist (renamed/removed).
    {
        const std::set<std::string> known(fields.begin(), fields.end());
        for (const auto& [field, entry] : allowlist()) {
            (void)entry;
            if (!known.count(field))
                failures.push_back("allowlist entry '" + field +
                                   "' matches no IRStyle/IRLayout/checked IRNode "
                                   "field — remove or rename the entry.");
        }
    }

    std::ostringstream report;
    report << "design-import four-surface parity violations ("
           << failures.size() << "):\n";
    for (const auto& f : failures) report << "  * " << f << "\n";
    report << "Surfaces: ";
    for (const auto& s : surfaces()) report << s.key << "=" << s.path << " ";
    INFO(report.str());
    REQUIRE(failures.empty());
}
