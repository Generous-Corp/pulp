// test_design_import_claude_runtime.cpp
//
// pulp #468 — exercises the `--execute-bundle` harness end-to-end.
// Boots a real ScriptEngine + WidgetBridge against synthetic Claude
// Design bundles built on the fly, then asserts the materialized DOM
// walker emits a DesignIR with materially more nodes than the
// loader-shell baseline (>30, per the current issue's success bar).
//
// Optional [.fixture] case runs against the real Spectr fixture when
// PULP_CLAUDE_BUNDLE_FIXTURE points at a Claude Design HTML on disk.

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

// Mirrors the builder in test_design_import_claude_bundle.cpp.
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
                case '/':  out += "\\u002F"; break;  // avoid </script> close
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
    ss << "<!DOCTYPE html><html><head><title>Test</title></head><body>"
       << "<script type=\"__bundler/manifest\">" << manifest_json << "</script>"
       << "<script type=\"__bundler/template\">" << json_quote(template_body_html)
       << "</script>"
       << "</body></html>";
    return ss.str();
}

} // namespace

TEST_CASE("parse_claude_html_with_runtime falls back when no bundler envelope present",
          "[view][import][issue-468]") {
    std::string err;
    ClaudeRuntimeOptions opts; opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        "<html><body><div>nothing here</div></body></html>", opts);
    // Falls back to static parser, which still produces something.
    REQUIRE(ir.source == DesignSource::claude);
    REQUIRE(err.find("no bundler envelope") != std::string::npos);
}

TEST_CASE("parse_claude_html_with_runtime materialises an app-injected DOM tree",
          "[view][import][issue-468]") {
    // Synthetic "app code" that mounts a non-trivial tree on document.body
    // when evaluated. Mimics what React's commit phase eventually does:
    // builds a hierarchy of named elements with classes, attributes,
    // styles, and text — enough for the walker + IR mapper to see >30
    // nodes.
    const std::string app_js = R"JS(
        if (globalThis.IS_REACT_ACT_ENVIRONMENT !== true) {
            throw new Error('IS_REACT_ACT_ENVIRONMENT was not enabled before app eval');
        }
        var root = document.getElementById('root');
        if (!root) {
            root = document.createElement('div');
            root.id = 'root';
            document.body.appendChild(root);
        }
        function make(tag, props, kids) {
            var el = document.createElement(tag);
            if (props) {
                for (var k in props) {
                    if (k === 'class') el.className = props[k];
                    else if (k === 'style') {
                        for (var s in props.style) el.style[s] = props.style[s];
                    } else if (k === 'text') el.textContent = props[k];
                    else el.setAttribute(k, props[k]);
                }
            }
            if (kids) for (var i = 0; i < kids.length; i++) el.appendChild(kids[i]);
            return el;
        }
        // Simulate a small editor: 3 panels, each with a label + button.
        var editor = make('div', { id: 'editor', 'data-pulp-role': 'editor-root' });
        var panels = [
            'Oscillator', 'Filter', 'Amp', 'Envelope', 'LFO', 'Matrix',
            'Drive', 'Mixer', 'Output', 'Modulation', 'Macros', 'Scope'
        ];
        for (var i = 0; i < panels.length; i++) {
            var name = panels[i];
            var panel = make('section', {
                'class': 'panel',
                'data-pulp-role': 'panel',
                style: { backgroundColor: '#1e1e2e', padding: '8px' }
            });
            panel.appendChild(make('h2', { text: name }));
            panel.appendChild(make('button', { 'data-pulp-action': 'tap-' + name }));
            editor.appendChild(panel);
        }
        root.appendChild(editor);
    )JS";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-app", "text/javascript", app_js, true) << "}";

    const std::string body =
        R"(<div id="root"></div><script src="u-app"></script>)";

    std::string err;
    ClaudeRuntimeOptions opts; opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(err.empty());
    REQUIRE(ir.source == DesignSource::claude);

    // Walk the materialized IR and count nodes — must beat the
    // loader-shell baseline of 30.
    std::function<size_t(const IRNode&)> count = [&](const IRNode& n) {
        size_t total = 1;
        for (const auto& c : n.children) total += count(c);
        return total;
    };
    auto nodes = count(ir.root);
    INFO("materialized IR node count: " << nodes);
    REQUIRE(nodes > 30);

    // Confirm at least one node carries the data-pulp-role marker the
    // app set — proves the attribute round-trip works.
    bool found_role = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end() && !it->second.empty()) found_role = true;
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(found_role);
}

TEST_CASE("parse_claude_html_with_runtime skips document chrome and maps inline layout",
          "[view][import][issue-1690]") {
    const std::string app_js = R"JS(
        var root = document.getElementById('root');
        root.style.position = 'absolute';
        root.style.inset = '0';
        root.style.display = 'flex';
        root.style.flexDirection = 'row';
        root.style.gap = '8px';
        root.style.padding = '6px 10px';
        root.style.justifyContent = 'space-between';
        root.style.alignItems = 'center';
        root.style.overflow = 'hidden';
        root.style.background = 'linear-gradient(90deg, #111111, #222222)';

        var button = document.createElement('button');
        button.id = 'run-button';
        button.style.flex = '2 1 auto';
        button.style.margin = '1px 2px 3px 4px';
        button.textContent = 'Run';
        root.appendChild(button);

        var label = document.createElement('span');
        label.id = 'status-label';
        label.textContent = 'OK';
        root.appendChild(label);

        for (var i = 0; i < 32; i++) {
            var cell = document.createElement('div');
            cell.id = 'cell-' + i;
            cell.textContent = '.';
            root.appendChild(cell);
        }
    )JS";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-app", "text/javascript", app_js, true) << "}";

    const std::string full_document_template = R"HTML(
        <!DOCTYPE html>
        <html>
          <head>
            <title>Hidden Spectr Title</title>
            <style>body { color: red; }</style>
          </head>
          <body>
            <div id="root"></div>
            <script src="u-app"></script>
          </body>
        </html>
    )HTML";

    std::string err;
    ClaudeRuntimeOptions opts; opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), full_document_template), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(err.empty());
    REQUIRE(ir.source == DesignSource::claude);

    std::function<size_t(const IRNode&)> count = [&](const IRNode& n) {
        size_t total = 1;
        for (const auto& c : n.children) total += count(c);
        return total;
    };
    REQUIRE(count(ir.root) > 30);

    bool found_hidden_title = false;
    bool emitted_head = false;
    std::function<void(const IRNode&)> scan = [&](const IRNode& n) {
        if (n.name == "head" || n.type == "head" || n.name == "html" || n.type == "html")
            emitted_head = true;
        if (n.text_content.find("Hidden Spectr Title") != std::string::npos)
            found_hidden_title = true;
        for (const auto& c : n.children) scan(c);
    };
    scan(ir.root);
    REQUIRE_FALSE(emitted_head);
    REQUIRE_FALSE(found_hidden_title);

    REQUIRE(ir.root.children.size() == 1);
    const auto& root = ir.root.children.front();
    REQUIRE(root.name == "root");
    REQUIRE(root.layout.direction == LayoutDirection::row);
    REQUIRE(root.layout.gap == 8.0f);
    REQUIRE(root.layout.padding_top == 6.0f);
    REQUIRE(root.layout.padding_right == 10.0f);
    REQUIRE(root.layout.padding_bottom == 6.0f);
    REQUIRE(root.layout.padding_left == 10.0f);
    REQUIRE(root.layout.justify == LayoutAlign::space_between);
    REQUIRE(root.layout.align == LayoutAlign::center);
    REQUIRE(root.layout.width_mode == SizingMode::fill);
    REQUIRE(root.layout.height_mode == SizingMode::fill);
    REQUIRE(root.style.position == "absolute");
    REQUIRE(root.style.overflow == "hidden");
    REQUIRE(root.style.background_gradient.has_value());
    REQUIRE(root.style.top == 0.0f);
    REQUIRE(root.style.right == 0.0f);
    REQUIRE(root.style.bottom == 0.0f);
    REQUIRE(root.style.left == 0.0f);

    const auto& button = root.children.front();
    REQUIRE(button.name == "run-button");
    REQUIRE(std::stof(button.attributes.at("_flexGrow")) == 2.0f);
    REQUIRE(std::stof(button.attributes.at("_marginTop")) == 1.0f);
    REQUIRE(std::stof(button.attributes.at("_marginRight")) == 2.0f);
    REQUIRE(std::stof(button.attributes.at("_marginBottom")) == 3.0f);
    REQUIRE(std::stof(button.attributes.at("_marginLeft")) == 4.0f);
}

TEST_CASE("parse_claude_html_with_runtime preserves inert JSON scripts for app render",
          "[view][import][issue-1690]") {
    const std::string app_js = R"JS(
        var cfg = JSON.parse(document.getElementById('tweak-defaults').textContent);
        var root = document.getElementById('root');
        for (var i = 0; i < cfg.cells; i++) {
            var d = document.createElement('div');
            d.id = 'json-cell-' + i;
            d.setAttribute('data-pulp-role', 'json-cell');
            root.appendChild(d);
        }
    )JS";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-app", "text/javascript", app_js, true) << "}";

    const std::string body =
        R"(<div id="root"></div>)"
        R"(<script type="application/json" id="tweak-defaults">{"cells":36}</script>)"
        R"(<script src="u-app"></script>)";

    std::string err;
    ClaudeRuntimeOptions opts; opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(err.empty());

    std::function<size_t(const IRNode&)> count = [&](const IRNode& n) {
        size_t total = 1;
        for (const auto& c : n.children) total += count(c);
        return total;
    };
    REQUIRE(count(ir.root) > 30);

    bool found_json_cell = false;
    bool emitted_script = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        if (n.type == "script") emitted_script = true;
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end() && it->second == "json-cell") found_json_cell = true;
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(found_json_cell);
    REQUIRE_FALSE(emitted_script);
}

TEST_CASE("parse_claude_html_with_runtime forces browser branch for UMD payloads",
          "[view][import][issue-1690]") {
    const std::string commonjs_globals = R"JS(
        globalThis.exports = {};
        globalThis.module = { exports: globalThis.exports };
    )JS";
    const std::string umd_payload = R"JS(
        (function(global, factory) {
            typeof exports === 'object' && typeof module !== 'undefined'
                ? factory(exports)
                : factory((global = global || self).BrowserLib = {});
        }(this, function(exports) {
            exports.ready = true;
        }));
    )JS";
    const std::string app_js = R"JS(
        if (!globalThis.BrowserLib || !globalThis.BrowserLib.ready) {
            throw new Error('BrowserLib did not materialize on globalThis');
        }
        var root = document.getElementById('root');
        for (var i = 0; i < 36; i++) {
            var d = document.createElement('div');
            d.id = 'umd-cell-' + i;
            d.setAttribute('data-pulp-role', 'umd-cell');
            root.appendChild(d);
        }
    )JS";

    std::ostringstream manifest;
    manifest << "{"
             << manifest_entry("u-commonjs", "text/javascript", commonjs_globals, true) << ","
             << manifest_entry("u-umd", "text/javascript", umd_payload, true) << ","
             << manifest_entry("u-app", "text/javascript", app_js, true)
             << "}";

    const std::string body =
        R"(<div id="root"></div>)"
        R"(<script src="u-commonjs"></script>)"
        R"(<script src="u-umd"></script>)"
        R"(<script src="u-app"></script>)";

    std::string err;
    ClaudeRuntimeOptions opts; opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(err.empty());

    bool found_umd_cell = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end() && it->second == "umd-cell") found_umd_cell = true;
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(found_umd_cell);
}

TEST_CASE("parse_claude_html_with_runtime falls back when bundle JS is too large",
          "[view][import][issue-468]") {
    // 1 KB payload — bigger than the configured cap below — so the
    // harness short-circuits to the static parser. Use compressed:false
    // here because the test harness's hand-rolled gzip wrap doesn't
    // round-trip cleanly through `deflate_decompress` for highly
    // compressible inputs (separately tracked).
    std::string big_js;
    big_js.reserve(2048);
    while (big_js.size() < 1024) big_js += "/* pad */ ";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-big", "text/javascript", big_js, false) << "}";
    const std::string body =
        R"(<div id="root"></div><script src="u-big"></script>)";

    auto envelope = build_envelope(manifest.str(), body);

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.max_total_js_bytes = 100;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(envelope, opts);
    INFO("err: " << err);
    REQUIRE(err.find("bundled JS too large") != std::string::npos);
    REQUIRE(ir.source == DesignSource::claude);
}

TEST_CASE("parse_claude_html_with_runtime falls back below the loader-shell floor",
          "[view][import][issue-468]") {
    // App that creates only one extra div — total IR ends up below the
    // 30-node floor, so the harness should fall back to the static
    // parser rather than return a regression.
    const std::string trivial_js = R"JS(
        var root = document.getElementById('root');
        if (!root) { root = document.createElement('div'); root.id = 'root'; document.body.appendChild(root); }
        root.appendChild(document.createElement('span'));
    )JS";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-tiny", "text/javascript", trivial_js, true) << "}";
    const std::string body = R"(<div id="root"></div><script src="u-tiny"></script>)";

    std::string err;
    ClaudeRuntimeOptions opts; opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);
    INFO("error_out: " << err);
    REQUIRE(err.find("loader-shell floor") != std::string::npos);
}

TEST_CASE("parse_claude_html_with_runtime against the real Spectr fixture "
          "when PULP_CLAUDE_BUNDLE_FIXTURE is set",
          "[view][import][issue-468][.fixture]") {
    const char* fixture = std::getenv("PULP_CLAUDE_BUNDLE_FIXTURE");
    if (!fixture || !*fixture) {
        SUCCEED("PULP_CLAUDE_BUNDLE_FIXTURE not set — skipping real-bundle harness test");
        return;
    }
    std::ifstream f(fixture);
    if (!f.is_open()) {
        SUCCEED("fixture not readable, skipping: " << fixture);
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    // Generous cap: real Spectr bundle is ~4.3 MB inflated.
    opts.max_total_js_bytes = 10 * 1024 * 1024;

    auto ir = parse_claude_html_with_runtime(ss.str(), opts);

    std::function<size_t(const IRNode&)> count = [&](const IRNode& n) {
        size_t total = 1;
        for (const auto& c : n.children) total += count(c);
        return total;
    };
    auto nodes = count(ir.root);
    INFO("materialized IR node count from Spectr fixture: " << nodes);
    INFO("error_out (empty=success): " << err);
    REQUIRE(ir.source == DesignSource::claude);
    // Acceptance bar from the issue: > 30 nodes (loader-shell baseline).
    // If even the real Spectr bundle can't get past the floor, surface
    // the error_out via INFO above so the diagnostic is captured.
    REQUIRE(nodes > 30);
}
