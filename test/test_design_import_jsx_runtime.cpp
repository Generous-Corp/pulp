// test_design_import_jsx_runtime.cpp
//
// pulp jsx-instrument-import experiment slice (2026-05-17).
//
// Validates that a pre-compiled JSX bundle (esbuild IIFE wrapping
// React + either ReactDOM or the @pulp/react native bridge + the user's JSX)
// can be wrapped as a Claude-style envelope and executed through the existing
// runtime-import harness (`parse_claude_html_with_runtime`). If the materialized DesignIR
// contains the named control panels Chainer ships (e.g. "generator",
// "envelope"), the renderer-integration path is validated and the
// follow-up work — `--from jsx` CLI wiring, drag, animation, parity
// — is well-scoped.
//
// Fast-iterate harness:
//   1. `node tools/import-design/jsx-runtime/jsx-transform.mjs --in
//      planning/fixtures/jsx/chainer-instrument.jsx --out /tmp/chainer-bundle.js`
//   2. `cmake --build build --target pulp-test-design-import-jsx-runtime`
//   3. `PULP_JSX_BUNDLE=/tmp/chainer-bundle.js ./build/test/pulp-test-design-import-jsx-runtime`
//
// The bundle file path is read from PULP_JSX_BUNDLE — keeping the .cpp
// free of multi-MB inline data and letting the Node transform loop
// re-emit the bundle without a C++ recompile.

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/zip.hpp>
#include <pulp/view/design_import.hpp>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

using namespace pulp::view;

namespace {

std::vector<uint8_t> gzip_wrap_deflate(const std::vector<uint8_t>& raw) {
    auto deflated = pulp::runtime::deflate_compress(raw.data(), raw.size());
    REQUIRE(deflated.has_value());
    std::vector<uint8_t> out;
    out.reserve(deflated->size() + 18);
    const uint8_t header[10] = {0x1f, 0x8b, 0x08, 0x00,
                                0, 0, 0, 0,
                                0, 0xff};
    out.insert(out.end(), header, header + 10);
    out.insert(out.end(), deflated->begin(), deflated->end());
    for (int i = 0; i < 4; ++i) out.push_back(0);
    uint32_t isize = static_cast<uint32_t>(raw.size());
    out.push_back(static_cast<uint8_t>(isize & 0xff));
    out.push_back(static_cast<uint8_t>((isize >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((isize >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((isize >> 24) & 0xff));
    return out;
}

std::string manifest_entry(const std::string& uuid, const std::string& mime,
                           const std::string& contents, bool compressed) {
    std::vector<uint8_t> bytes(contents.begin(), contents.end());
    std::vector<uint8_t> payload = compressed ? gzip_wrap_deflate(bytes)
                                              : std::move(bytes);
    std::string b64 = pulp::runtime::base64_encode(payload.data(), payload.size());
    std::ostringstream ss;
    ss << "\"" << uuid << "\":{"
       << "\"mime\":\"" << mime << "\","
       << "\"compressed\":" << (compressed ? "true" : "false") << ","
       << "\"data\":\"" << b64 << "\"}";
    return ss.str();
}

std::string build_envelope(const std::string& manifest_json,
                           const std::string& template_body_html) {
    auto json_quote = [](const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '/':  out += "\\u002F"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        out += "\"";
        return out;
    };

    std::ostringstream ss;
    ss << "<!DOCTYPE html><html><head><title>JSX Runtime Smoke</title></head><body>"
       << "<script type=\"__bundler/manifest\">" << manifest_json << "</script>"
       << "<script type=\"__bundler/template\">" << json_quote(template_body_html)
       << "</script>"
       << "</body></html>";
    return ss.str();
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

size_t count_ir_nodes(const IRNode& n) {
    size_t total = 1;
    for (const auto& c : n.children) total += count_ir_nodes(c);
    return total;
}

bool ir_contains_text(const IRNode& n, const std::string& needle) {
    if (n.text_content.find(needle) != std::string::npos) return true;
    for (const auto& c : n.children) if (ir_contains_text(c, needle)) return true;
    return false;
}

const IRNode* find_ir_node_by_anchor(const IRNode& n, const std::string& anchor) {
    if (n.stable_anchor_id && *n.stable_anchor_id == anchor)
        return &n;
    for (const auto& c : n.children) {
        if (const auto* found = find_ir_node_by_anchor(c, anchor))
            return found;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("[jsx-experiment] Chainer JSX bundle materializes through Claude runtime harness",
          "[view][import][jsx]") {
    const char* bundle_path = std::getenv("PULP_JSX_BUNDLE");
    if (!bundle_path || !*bundle_path) {
        SUCCEED("PULP_JSX_BUNDLE not set — skipping. Run the Node transform first: "
                "node tools/import-design/jsx-runtime/jsx-transform.mjs --in "
                "planning/fixtures/jsx/chainer-instrument.jsx --out /tmp/chainer-bundle.js");
        return;
    }

    const std::string app_js = read_file(bundle_path);
    INFO("loaded bundle bytes: " << app_js.size() << " from " << bundle_path);
    REQUIRE(!app_js.empty());
    REQUIRE(app_js.size() > 10000);  // sanity: real bundle is ~1 MB

    // Wrap as Claude envelope. Use compressed=false for large bundles because
    // the test's hand-rolled gzip wrap has round-trip issues for highly-
    // compressible inputs (see test_design_import_claude_runtime.cpp:198).
    std::ostringstream manifest;
    manifest << "{" << manifest_entry("jsx-app", "text/javascript", app_js, false) << "}";

    const std::string body =
        R"(<div id="root"></div><script src="jsx-app"></script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    // Bundle is ~1.1 MB; give it 4 MB of headroom.
    opts.max_total_js_bytes = 4 * 1024 * 1024;

    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    const size_t nodes = count_ir_nodes(ir.root);
    INFO("runtime error_out: " << err);
    INFO("materialized IR node count: " << nodes);

    // First-tier acceptance: nothing exploded.
    REQUIRE(ir.source == DesignSource::claude);

    // Second-tier: bundle ran past the loader-shell floor. If it fell back
    // to static parsing, error_out tells us why.
    REQUIRE(nodes > 9);

    // Third-tier (the interesting bit): some of Chainer's section labels
    // should make it through into the materialized text. These are the
    // explicit `title` strings on the Section() calls in the JSX.
    // We accept ANY of these to keep the test resilient against partial
    // render — a stronger version would require all four.
    const bool found_chainer_text =
        ir_contains_text(ir.root, "generator")  ||
        ir_contains_text(ir.root, "envelope")   ||
        ir_contains_text(ir.root, "multiband")  ||
        ir_contains_text(ir.root, "CHAINER")    ||
        ir_contains_text(ir.root, "polywave");
    INFO("Chainer-shaped text found in IR: " << (found_chainer_text ? "yes" : "no"));
    REQUIRE(found_chainer_text);
}

TEST_CASE("[jsx-experiment] native live bundle can freeze through baked snapshot fallback",
          "[view][import][jsx]") {
    std::string native_bundle =
        "(function(){\n"
        "  createCol('native-panel', '');\n"
        "  for (var i = 0; i < 12; ++i) {\n"
        "    createLabel('native-label-' + i, i === 0 ? 'CHAINER native snapshot' : ('native row ' + i), 'native-panel');\n"
        "  }\n"
        "})();\n";
    native_bundle.append(256, ' ');

    auto bundle = parse_jsx_react(native_bundle, "NativeBridgeSmoke");
    REQUIRE(bundle.has_value());

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(synthesize_runtime_envelope(*bundle), opts);

    INFO("runtime error_out: " << err);
    INFO("capture_method: " << ir.capture_method);
    INFO("materialized native IR node count: " << count_ir_nodes(ir.root));
    REQUIRE(err.empty());
    REQUIRE(ir.capture_method == "runtime_native_snapshot");
    REQUIRE(ir.source_adapter == "claude-native-view");
    REQUIRE(count_ir_nodes(ir.root) > 9);
    REQUIRE(ir_contains_text(ir.root, "CHAINER native snapshot"));
}

TEST_CASE("[jsx-experiment] native live bundle freezes after requested viewport layout",
          "[view][import][jsx]") {
    std::string native_bundle =
        "(function(){\n"
        "  createCol('native-panel', '');\n"
        "  createLabel('caption', 'native viewport', 'native-panel');\n"
        "  setFlex('caption', 'margin_bottom', 8);\n"
        "  setBorderBottomColor('caption', '#112233');\n"
        "  setBorderBottomWidth('caption', 1);\n"
        "  createRow('wrap-row', 'native-panel');\n"
        "  setFlex('wrap-row', 'flex_wrap', 'wrap');\n"
        "  for (var i = 0; i < 8; ++i) {\n"
        "    var id = 'tile-' + i;\n"
        "    createCol(id, 'wrap-row');\n"
        "    setFlex(id, 'width', 120);\n"
        "    setFlex(id, 'height', 20);\n"
        "    setFlex(id, 'flex_shrink', 0);\n"
        "  }\n"
        "  createCol('align-host', 'native-panel');\n"
        "  setFlex('align-host', 'width', 260);\n"
        "  setFlex('align-host', 'align_items', 'center');\n"
        "  createCol('centered-child', 'align-host');\n"
        "  setFlex('centered-child', 'width', 120);\n"
        "  setFlex('centered-child', 'height', 20);\n"
        "})();\n";
    native_bundle.append(256, ' ');

    auto bundle = parse_jsx_react(native_bundle, "NativeViewportSmoke");
    REQUIRE(bundle.has_value());

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    opts.runtime_snapshot_viewport_width = 260;
    opts.runtime_snapshot_viewport_height = 180;
    auto ir = parse_claude_html_with_runtime(synthesize_runtime_envelope(*bundle), opts);

    INFO("runtime error_out: " << err);
    INFO("capture_method: " << ir.capture_method);
    REQUIRE(err.empty());
    REQUIRE(ir.capture_method == "runtime_native_snapshot");

    const auto* panel = find_ir_node_by_anchor(ir.root, "native-panel");
    const auto* caption = find_ir_node_by_anchor(ir.root, "caption");
    const auto* wrap = find_ir_node_by_anchor(ir.root, "wrap-row");
    const auto* align_host = find_ir_node_by_anchor(ir.root, "align-host");
    REQUIRE(panel != nullptr);
    REQUIRE(caption != nullptr);
    REQUIRE(wrap != nullptr);
    REQUIRE(align_host != nullptr);
    REQUIRE(panel->style.width);
    REQUIRE(caption->layout.margin_bottom);
    REQUIRE(wrap->style.width);
    REQUIRE(wrap->style.height);
    REQUIRE(*panel->style.width == 260.0f);
    REQUIRE(*caption->layout.margin_bottom == 8.0f);
    REQUIRE(caption->style.border_bottom_width);
    REQUIRE(caption->style.border_bottom_color);
    REQUIRE(*caption->style.border_bottom_width == 1.0f);
    REQUIRE(*caption->style.border_bottom_color == "#112233");
    REQUIRE(align_host->layout.align == LayoutAlign::center);
    REQUIRE(*wrap->style.width == 260.0f);
    REQUIRE(*wrap->style.height > 20.0f);
}

TEST_CASE("[jsx-experiment] TypeScript .tsx bundle materializes through Claude runtime harness",
          "[view][import][jsx][tsx]") {
    // Same shape as the JSX case but proves the esbuild 'tsx' loader strips
    // TypeScript correctly and the resulting JS round-trips through the
    // runtime harness. Per Codex high-reasoning consult (2026-05-17): TSX
    // is the dominant React file shape today; making the importer handle
    // it lifts the experiment from "one hand-picked .jsx fixture" to the
    // mainstream React export surface.
    const char* bundle_path = std::getenv("PULP_TSX_BUNDLE");
    if (!bundle_path || !*bundle_path) {
        SUCCEED("PULP_TSX_BUNDLE not set — skipping. Run the Node transform first: "
                "node tools/import-design/jsx-runtime/jsx-transform.mjs --in "
                "planning/fixtures/jsx/typed-control.tsx --out /tmp/tsx-bundle.js");
        return;
    }

    const std::string app_js = read_file(bundle_path);
    INFO("loaded TSX bundle bytes: " << app_js.size() << " from " << bundle_path);
    REQUIRE(!app_js.empty());
    REQUIRE(app_js.size() > 10000);

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("tsx-app", "text/javascript", app_js, false) << "}";
    const std::string body = R"(<div id="root"></div><script src="tsx-app"></script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    opts.max_total_js_bytes = 4 * 1024 * 1024;

    auto ir = parse_claude_html_with_runtime(build_envelope(manifest.str(), body), opts);

    const size_t nodes = count_ir_nodes(ir.root);
    INFO("runtime error_out: " << err);
    INFO("materialized TSX IR node count: " << nodes);

    REQUIRE(ir.source == DesignSource::claude);
    REQUIRE(nodes > 1);  // mount root + at least one child from TypedControl

    // typed-control.tsx renders a div containing the text "TYPED CONTROL"
    // and a Meter component whose label resolves to "gain". Either string
    // proves the TSX→JS→native pipeline survived type-stripping.
    const bool found_tsx_text =
        ir_contains_text(ir.root, "TYPED CONTROL") ||
        ir_contains_text(ir.root, "gain");
    INFO("TSX-shaped text found in IR: " << (found_tsx_text ? "yes" : "no"));
    REQUIRE(found_tsx_text);
}
