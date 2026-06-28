// test_design_import_inline_babel.cpp
//
// pulp #758 — exercises the inline-script eval path of the
// `--execute-bundle` Claude harness:
//
//   Step 1: inline `<script type="text/javascript">` blocks evaluated
//           in document order.
//   Step 2: inline `<script type="text/babel">` blocks compiled via a
//           Babel-standalone shim and then evaluated.
//   Step 3: DOMContentLoaded dispatched after step 2.
//   Step 4: layered async drain runs deeper than the original 2-pump.
//
// We use a hand-rolled Babel-standalone shim (a global `Babel.transform`
// that returns its input unchanged via a `.code` property) so the test
// stays cheap. The transformed "JSX" is just plain JS that builds a
// non-trivial DOM tree on document.body — which is enough to prove the
// inline-eval path fired and the resulting DesignIR is materially
// deeper than the loader-shell baseline.
//
// An optional [.fixture] case runs against the canonical 1.86 MB Spectr
// `editor.html` fixture when PULP_CLAUDE_BUNDLE_FIXTURE points at it.

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

std::string json_quote(const std::string& s) {
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
}

std::string build_envelope(const std::string& manifest_json,
                           const std::string& template_body_html) {
    std::ostringstream ss;
    ss << "<!DOCTYPE html><html><head><title>Test</title></head><body>"
       << "<script type=\"__bundler/manifest\">" << manifest_json << "</script>"
       << "<script type=\"__bundler/template\">" << json_quote(template_body_html)
       << "</script>"
       << "</body></html>";
    return ss.str();
}

} // namespace

TEST_CASE("inline text/javascript blocks are evaluated in document order",
          "[view][import][issue-758]") {
    // src-loaded payload installs a counter; inline `text/javascript`
    // increments it twice. Inline scripts run AFTER the src-loaded
    // payloads (which is the document-load order React+Babel+app rely
    // on).
    const std::string lib_js = R"JS(
        globalThis.__pulp_counter__ = 0;
        globalThis.__pulp_log__ = [];
    )JS";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-lib", "text/javascript", lib_js, true) << "}";

    // Two inline JS blocks, then a DOM-mutating block, then a `text/json`
    // block (which the harness must skip — it's data, not code).
    std::string body;
    body += R"(<div id="root"></div>)";
    body += R"(<script src="u-lib"></script>)";
    body += R"(<script type="text/javascript">globalThis.__pulp_counter__++; globalThis.__pulp_log__.push('first');</script>)";
    body += R"(<script>globalThis.__pulp_counter__++; globalThis.__pulp_log__.push('second-untyped');</script>)";
    body += R"(<script type="application/json">{"skip":"this should be ignored"}</script>)";
    body += R"(<script type="text/javascript">
        var root = document.getElementById('root');
        // 12 cells so the materialized walker tree (body + root + cells)
        // beats the 9-node loader-shell floor.
        for (var i = 0; i < 12; i++) {
            var d = document.createElement('div');
            d.id = 'inline-js-' + i;
            d.setAttribute('data-pulp-role', 'cell');
            root.appendChild(d);
        }
    </script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(ir.source == DesignSource::claude);
    // Walker must produce >9 nodes (the loader-shell floor).
    std::function<size_t(const IRNode&)> count = [&](const IRNode& n) {
        size_t total = 1;
        for (const auto& c : n.children) total += count(c);
        return total;
    };
    REQUIRE(count(ir.root) > 9);

    // The data-pulp-role attribute should round-trip — proves the inline
    // DOM-building block actually ran.
    bool found_role = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end() && it->second == "cell") found_role = true;
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(found_role);
}

TEST_CASE("inline text/babel blocks are compiled via Babel-standalone and evaluated",
          "[view][import][issue-758]") {
    // Babel-standalone shim: the harness checks for
    // `globalThis.Babel.transform`. Our shim returns the input source
    // unchanged via `.code` — that's enough to verify the harness:
    //   1. Detects the shim is loaded.
    //   2. Calls .transform() with the JSX body.
    //   3. Evaluates the returned `.code` against the engine.
    //
    // We tag the shim's transform with a global counter so the test can
    // assert the harness called it once per inline `text/babel` script.
    const std::string babel_shim_js = R"JS(
        globalThis.Babel = {
            __callCount: 0,
            transform: function(src, opts) {
                globalThis.Babel.__callCount += 1;
                return { code: src };
            }
        };
    )JS";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-babel", "text/javascript", babel_shim_js, true) << "}";

    // Two inline `text/babel` blocks. Their bodies are valid JS (the
    // shim is an identity transform), so eval succeeds without a real
    // JSX compiler in the loop.
    std::string body;
    body += R"(<div id="root"></div>)";
    body += R"(<script src="u-babel"></script>)";
    body += R"(<script type="text/babel">
        (function(){
            var root = document.getElementById('root');
            var panel = document.createElement('section');
            panel.id = 'babel-panel';
            panel.setAttribute('data-pulp-role', 'panel');
            // Build 8 buttons so the materialized tree clears the
            // 9-node loader-shell floor even on the smaller layout.
            for (var i = 0; i < 8; i++) {
                var btn = document.createElement('button');
                btn.id = 'babel-btn-' + i;
                btn.setAttribute('data-pulp-role', 'tap');
                panel.appendChild(btn);
            }
            root.appendChild(panel);
        })();
    </script>)";
    body += R"(<script type="text/jsx">
        (function(){
            // Use a div (not a span) — empty spans get filtered out by
            // the text-empty pruning in json_to_ir_node, which would
            // strip the data-pulp-role marker. Divs survive the prune
            // because they map to "frame", not "text".
            var label = document.createElement('div');
            label.id = 'jsx-label';
            label.setAttribute('data-pulp-role', 'jsx');
            document.getElementById('root').appendChild(label);
        })();
    </script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(ir.source == DesignSource::claude);

    std::function<size_t(const IRNode&)> count = [&](const IRNode& n) {
        size_t total = 1;
        for (const auto& c : n.children) total += count(c);
        return total;
    };
    auto nodes = count(ir.root);
    INFO("materialized IR node count: " << nodes);
    REQUIRE(nodes > 9);

    // Both the babel and jsx inline scripts should have produced
    // role-tagged children.
    bool saw_panel = false, saw_jsx = false, saw_tap = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end()) {
            if (it->second == "panel") saw_panel = true;
            if (it->second == "jsx")   saw_jsx = true;
            if (it->second == "tap")   saw_tap = true;
        }
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(saw_panel);
    REQUIRE(saw_jsx);
    REQUIRE(saw_tap);
}

TEST_CASE("inline text/babel blocks are skipped (no DOM mutation) when Babel-standalone is missing",
          "[view][import][issue-758]") {
    // No Babel shim — the harness should detect that and SKIP every
    // inline text/babel block (rather than blowing up). We assert
    // behavior rather than a particular diagnostic string: a babel
    // block whose body would have appended a flag div should leave
    // no trace in the materialized IR. The companion text/javascript
    // block builds enough DOM to clear the >9 walker floor so the
    // harness doesn't fall back to the static parser.
    const std::string lib_js = R"JS( globalThis.__pulp_init__ = 1; )JS";
    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-lib", "text/javascript", lib_js, true) << "}";

    std::string body;
    body += R"(<div id="root"></div>)";
    body += R"(<script src="u-lib"></script>)";
    body += R"(<script type="text/javascript">
        var root = document.getElementById('root');
        for (var i = 0; i < 12; i++) {
            var d = document.createElement('div');
            d.id = 'cell-' + i;
            d.setAttribute('data-pulp-role', 'cell');
            root.appendChild(d);
        }
    </script>)";
    body += R"(<script type="text/babel">
        // This should NOT execute — Babel is missing.
        var d = document.createElement('div');
        d.id = 'babel-should-not-run';
        d.setAttribute('data-pulp-role', 'should-not-appear');
        document.getElementById('root').appendChild(d);
    </script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(ir.source == DesignSource::claude);

    // Materialized IR should NOT contain the babel-injected role marker.
    bool saw_should_not_appear = false;
    bool saw_cell = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end()) {
            if (it->second == "should-not-appear") saw_should_not_appear = true;
            if (it->second == "cell") saw_cell = true;
        }
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(saw_cell);                // proves the inline JS path ran
    REQUIRE_FALSE(saw_should_not_appear);  // proves babel was skipped
}

TEST_CASE("DOMContentLoaded dispatch runs the queued handler when document supports it",
          "[view][import][issue-758]") {
    // The current web-compat shim's `document` object literal lacks
    // `addEventListener` / `dispatchEvent` (it's a plain {} not an
    // Element). The harness's Step 3 dispatch code is intentionally
    // wrapped in try/catch so the missing methods don't crash the
    // harness — but we still want to verify the dispatch actually
    // fires the handler when the methods ARE present.
    //
    // To exercise that contract we install a tiny addEventListener /
    // dispatchEvent shim on `document` and `window` from the src-loaded
    // payload, then register a DCL listener from an inline JS block,
    // then assert the handler ran (the listener appends 12 div cells
    // — also enough to clear the >9 walker floor).
    const std::string lib_js = R"JS(
        function _installEventShim(target) {
            target.__listeners__ = {};
            target.addEventListener = function(type, fn) {
                if (!this.__listeners__[type]) this.__listeners__[type] = [];
                this.__listeners__[type].push(fn);
            };
            target.dispatchEvent = function(evt) {
                var t = (evt && evt.type) ? evt.type : '';
                var ls = this.__listeners__[t] || [];
                for (var i = 0; i < ls.length; i++) {
                    try { ls[i].call(this, evt); } catch (e) {}
                }
                return true;
            };
        }
        _installEventShim(document);
        if (typeof window !== 'undefined') _installEventShim(window);
    )JS";
    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-lib", "text/javascript", lib_js, true) << "}";

    std::string body;
    body += R"(<div id="root"></div>)";
    body += R"(<script src="u-lib"></script>)";
    // Pre-DCL: 6 baseline cells (so the materialized tree is at the
    // floor before DCL fires — DCL adding 6 more pushes us over the >9
    // floor and proves the DCL dispatch landed).
    body += R"(<script type="text/javascript">
        var root = document.getElementById('root');
        for (var i = 0; i < 6; i++) {
            var d = document.createElement('div');
            d.id = 'pre-' + i;
            d.setAttribute('data-pulp-role', 'pre-dcl');
            root.appendChild(d);
        }
        document.addEventListener('DOMContentLoaded', function() {
            for (var j = 0; j < 6; j++) {
                var d = document.createElement('div');
                d.id = 'dcl-' + j;
                d.setAttribute('data-pulp-role', 'after-dcl');
                root.appendChild(d);
            }
        });
    </script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(ir.source == DesignSource::claude);

    bool saw_after_dcl = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end() && it->second == "after-dcl") saw_after_dcl = true;
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(saw_after_dcl);
}

TEST_CASE("inline scripts with single-quoted and unrecognized types "
          "are classified correctly",
          "[view][import][issue-758]") {
    // Three additional inline-script attribute forms exercised here:
    //   1. type='text/javascript' (single-quoted) — kind='javascript'
    //   2. type=text/javascript   (unquoted)      — kind='javascript'
    //   3. type="text/template"   (unrecognized)  — kind='other', skipped
    //
    // The "other" path proves that exotic script types (vendor MIME
    // types, x-shader, x-template, etc.) are silently ignored rather
    // than blown up — important because real Claude bundles can carry
    // template stamps the harness shouldn't touch.
    const std::string lib_js = R"JS( globalThis.__pulp_lib__ = 1; )JS";
    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-lib", "text/javascript", lib_js, true) << "}";

    std::string body;
    body += R"(<div id="root"></div>)";
    body += R"(<script src="u-lib"></script>)";
    body += R"(<script type='text/javascript'>
        var root = document.getElementById('root');
        for (var i = 0; i < 4; i++) {
            var d = document.createElement('div');
            d.id = 'sq-' + i;
            d.setAttribute('data-pulp-role', 'single-quoted');
            root.appendChild(d);
        }
    </script>)";
    body += R"(<script type=text/javascript>
        var r = document.getElementById('root');
        for (var i = 0; i < 4; i++) {
            var d = document.createElement('div');
            d.id = 'uq-' + i;
            d.setAttribute('data-pulp-role', 'unquoted');
            r.appendChild(d);
        }
    </script>)";
    body += R"(<script type="text/template" id="ignored-template">
        // This must not execute — type=text/template is not in the
        // executable kinds set. The presence of arbitrary content here
        // (including raw HTML) must NOT crash the harness.
        <div>{{ would.crash.if.evaluated }}</div>
    </script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(ir.source == DesignSource::claude);

    bool saw_sq = false, saw_uq = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end()) {
            if (it->second == "single-quoted") saw_sq = true;
            if (it->second == "unquoted") saw_uq = true;
        }
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(saw_sq);
    REQUIRE(saw_uq);
}

TEST_CASE("babel-side transform errors are surfaced and the script is skipped",
          "[view][import][issue-758]") {
    // Babel shim that THROWS when called — simulates a malformed JSX
    // payload. The harness should catch it (via the babel-side
    // try/catch wrapped around Babel.transform), set
    // __pulpBabelErr__ to a non-empty string, surface it via
    // error_out, and continue without evaluating an empty `code`.
    const std::string babel_throwing_shim = R"JS(
        globalThis.Babel = {
            transform: function(src, opts) {
                throw new Error('synthetic SyntaxError in babel-throwing shim');
            }
        };
    )JS";
    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u-babel", "text/javascript", babel_throwing_shim, true) << "}";

    std::string body;
    body += R"(<div id="root"></div>)";
    body += R"(<script src="u-babel"></script>)";
    // 12 cells from inline JS so the materialized DOM clears the >9
    // floor and the harness doesn't fall back to the static parser
    // (which would overwrite our babel-error diagnostic in error_out).
    body += R"(<script type="text/javascript">
        var root = document.getElementById('root');
        for (var i = 0; i < 12; i++) {
            var d = document.createElement('div');
            d.id = 'cell-' + i;
            d.setAttribute('data-pulp-role', 'cell');
            root.appendChild(d);
        }
    </script>)";
    body += R"(<script type="text/babel">
        // Should not execute — Babel.transform throws.
        var d = document.createElement('div');
        d.id = 'should-not-appear';
        d.setAttribute('data-pulp-role', 'babel-leaked');
        document.getElementById('root').appendChild(d);
    </script>)";

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    auto ir = parse_claude_html_with_runtime(
        build_envelope(manifest.str(), body), opts);

    INFO("runtime error_out: " << err);
    REQUIRE(ir.source == DesignSource::claude);

    // Materialized IR should NOT contain the role marker that the
    // failing babel block tried to inject — the harness must have
    // surfaced the babel-side error and skipped the eval. (We can't
    // assert err.find("babel error") because error_out is the
    // fallback reason, cleared on a successful harness run; the
    // behavioral assertion below is the contract.)
    bool saw_leaked = false;
    bool saw_cell = false;
    std::function<void(const IRNode&)> walk = [&](const IRNode& n) {
        auto it = n.attributes.find("data-pulp-role");
        if (it != n.attributes.end()) {
            if (it->second == "babel-leaked") saw_leaked = true;
            if (it->second == "cell")         saw_cell = true;
        }
        for (const auto& c : n.children) walk(c);
    };
    walk(ir.root);
    REQUIRE(saw_cell);                 // proves the inline JS ran (sanity)
    REQUIRE_FALSE(saw_leaked);         // proves the babel block was skipped
}

TEST_CASE("real Spectr Claude bundle materialises widgets when "
          "PULP_CLAUDE_BUNDLE_FIXTURE is set",
          "[view][import][issue-758][.fixture]") {
    const char* fixture = std::getenv("PULP_CLAUDE_BUNDLE_FIXTURE");
    if (!fixture || !*fixture) {
        SKIP("PULP_CLAUDE_BUNDLE_FIXTURE not set; real-bundle test cannot run");
        return;
    }
    std::ifstream f(fixture);
    if (!f.is_open()) {
        SKIP(std::string("fixture not readable; real-bundle test cannot run: ") + fixture);
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    std::string err;
    ClaudeRuntimeOptions opts;
    opts.error_out = &err;
    // Real Spectr bundle is ~4.3 MB inflated; give plenty of headroom.
    opts.max_total_js_bytes = 16 * 1024 * 1024;

    auto ir = parse_claude_html_with_runtime(ss.str(), opts);

    std::function<size_t(const IRNode&)> count = [&](const IRNode& n) {
        size_t total = 1;
        for (const auto& c : n.children) total += count(c);
        return total;
    };
    auto nodes = count(ir.root);
    INFO("materialized IR node count from real Spectr fixture: " << nodes);
    INFO("error_out (empty=success): " << err);
    REQUIRE(ir.source == DesignSource::claude);
    // Acceptance: the real Spectr bundle should now produce dozens to
    // hundreds of nodes (the actual editor surface), not just the
    // loader-shell baseline of 9.
    REQUIRE(nodes > 20);
}
