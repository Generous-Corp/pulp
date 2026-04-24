// test_web_compat_react_shims.cpp
//
// Pulp #468 — verify the web-compat preludes expose the surface React 18
// (and similar bundled-React frameworks) feature-detect against. Each of
// these checks would have caught a regression that breaks bundled-React
// import via `pulp import-design --from claude --execute-bundle`.
//
// Methodology: spin up a real WidgetBridge so all preludes are evaluated
// in the same order they ship at runtime, then run a small JS snippet
// that exercises the surface and writes a one-line marker into a global
// the test reads back via load_script. We reuse the existing canvas
// command-recording pattern so we don't re-implement bridge plumbing.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

using namespace pulp::view;
using namespace pulp::state;

namespace {

// Build a minimal bridge so all web-compat preludes load (the bridge
// constructor is what evaluates them). Returns the script result of the
// final expression (a stringified marker our tests check).
std::string run_in_bridge(const std::string& js) {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    // load_script appends ";void 0" so we can't read the expression
    // value directly. Stash the result in a global and read it back.
    bridge.load_script("globalThis.__test_result__ = (function(){\n"
                       + js +
                       "\n})();");
    auto val = engine.evaluate("String(globalThis.__test_result__)");
    if (val.isString()) return std::string(val.getString());
    return std::string{};
}

} // namespace

// ── nodeType / nodeName on Element ──────────────────────────────────────

TEST_CASE("Element nodes report nodeType=1 and nodeName=tagName",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var div = document.createElement('div');
        var span = document.createElement('span');
        return [div.nodeType, div.nodeName, span.nodeType, span.nodeName].join('|');
    )");
    REQUIRE(result == "1|DIV|1|SPAN");
}

TEST_CASE("Element constructor + prototype expose DOM Level 1 node-type constants",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var d = document.createElement('div');
        return [
            Element.ELEMENT_NODE,
            Element.TEXT_NODE,
            Element.COMMENT_NODE,
            d.ELEMENT_NODE,
            d.TEXT_NODE,
            d.COMMENT_NODE
        ].join(',');
    )");
    REQUIRE(result == "1,3,8,1,3,8");
}

// ── createTextNode reports as a real text node ──────────────────────────

TEST_CASE("createTextNode reports nodeType=3 and nodeName='#text'",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var t = document.createTextNode('hello');
        return [t.nodeType, t.nodeName, t.data, t.nodeValue].join('|');
    )");
    REQUIRE(result == "3|#text|hello|hello");
}

TEST_CASE("Text node data setter mirrors nodeValue and updates _textContent",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var t = document.createTextNode('initial');
        t.data = 'updated';
        return [t.data, t.nodeValue, t._textContent].join('|');
    )");
    REQUIRE(result == "updated|updated|updated");
}

// ── createComment ───────────────────────────────────────────────────────

TEST_CASE("createComment reports nodeType=8 and nodeName='#comment'",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var c = document.createComment('hidden marker');
        return [c.nodeType, c.nodeName, c.data].join('|');
    )");
    REQUIRE(result == "8|#comment|hidden marker");
}

// ── createDocumentFragment ──────────────────────────────────────────────

TEST_CASE("createDocumentFragment reports nodeType=11",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var f = document.createDocumentFragment();
        return [f.nodeType, f.nodeName].join('|');
    )");
    REQUIRE(result == "11|#document-fragment");
}

// Codex P1 on PR #730: DocumentFragment MUST flatten on insert — the
// fragment's children move into the parent and the fragment node itself
// is never inserted. React 18's reconciler stages commits in fragments,
// so without this the materialized tree has phantom wrapper divs.
TEST_CASE("appendChild(fragment) flattens fragment children into the parent",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var parent = document.createElement('div');
        var frag = document.createDocumentFragment();
        var a = document.createElement('span');
        var b = document.createElement('span');
        var c = document.createElement('span');
        frag.appendChild(a);
        frag.appendChild(b);
        frag.appendChild(c);
        var before = frag._children.length;
        parent.appendChild(frag);
        return [
            'before=' + before,
            'parent=' + parent._children.length,
            'frag=' + frag._children.length,
            'first=' + (parent._children[0] === a),
            'last=' + (parent._children[2] === c),
            'parentOfA=' + (a._parentElement === parent)
        ].join(',');
    )");
    REQUIRE(result ==
            "before=3,parent=3,frag=0,first=true,last=true,parentOfA=true");
}

TEST_CASE("insertBefore(fragment) flattens fragment children in order",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var parent = document.createElement('div');
        var first = document.createElement('span');
        var third = document.createElement('span');
        parent.appendChild(first);
        parent.appendChild(third);
        var frag = document.createDocumentFragment();
        var a = document.createElement('span');
        var b = document.createElement('span');
        frag.appendChild(a);
        frag.appendChild(b);
        // Insert the fragment before `third` — expect first, a, b, third.
        parent.insertBefore(frag, third);
        return [
            'count=' + parent._children.length,
            'order=' +
                (parent._children[0] === first) + ',' +
                (parent._children[1] === a) + ',' +
                (parent._children[2] === b) + ',' +
                (parent._children[3] === third),
            'frag=' + frag._children.length
        ].join('|');
    )");
    REQUIRE(result == "count=4|order=true,true,true,true|frag=0");
}

// ── Observer constructors exist and are no-ops ──────────────────────────

TEST_CASE("MutationObserver / IntersectionObserver / ResizeObserver / PerformanceObserver are constructible no-ops",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var names = ['MutationObserver','IntersectionObserver','ResizeObserver','PerformanceObserver'];
        var ok = [];
        for (var i = 0; i < names.length; i++) {
            var Ctor = globalThis[names[i]];
            if (typeof Ctor !== 'function') { ok.push(names[i] + ':missing'); continue; }
            var inst = new Ctor(function(){});
            inst.observe();   // no throw
            inst.disconnect();
            inst.takeRecords();
            ok.push(names[i] + ':ok');
        }
        return ok.join(',');
    )");
    REQUIRE(result ==
            "MutationObserver:ok,IntersectionObserver:ok,ResizeObserver:ok,PerformanceObserver:ok");
}

TEST_CASE("XMLHttpRequest is a constructible no-op with the spec readyState constants",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        if (typeof XMLHttpRequest !== 'function') return 'missing';
        var xhr = new XMLHttpRequest();
        xhr.open('GET', '/x');
        xhr.send();
        return [
            XMLHttpRequest.UNSENT, XMLHttpRequest.OPENED,
            XMLHttpRequest.HEADERS_RECEIVED, XMLHttpRequest.LOADING,
            XMLHttpRequest.DONE
        ].join(',');
    )");
    REQUIRE(result == "0,1,2,3,4");
}

TEST_CASE("Element exposes a no-op scrollTop/scrollLeft pair returning 0",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var d = document.createElement('div');
        d.scrollTop = 99;   // setter is no-op
        d.scrollLeft = 99;
        return [d.scrollTop, d.scrollLeft].join(',');
    )");
    REQUIRE(result == "0,0");
}

// ── queueMicrotask ──────────────────────────────────────────────────────

TEST_CASE("queueMicrotask is exposed as a function on globalThis",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge("return typeof queueMicrotask;");
    REQUIRE(result == "function");
}

TEST_CASE("queueMicrotask invocation does not throw and the callback is registered",
          "[view][web-compat][issue-468]") {
    // queueMicrotask schedules through Promise.resolve.then. Actual
    // draining depends on the engine's job loop (CHOC's QuickJS
    // pumpMessageLoop is currently a no-op), so the contract we can
    // reliably test is: the global exists, is callable, accepts a
    // function argument, and its own validation path (reject non-
    // functions silently) behaves per spec. Each of those is what React
    // 18's `typeof queueMicrotask === 'function'` probe reads.
    auto result = run_in_bridge(R"(
        var threw = false;
        try {
            queueMicrotask(function () {});
            queueMicrotask();             // non-function: silently ignored
        } catch (e) { threw = true; }
        return threw ? 'threw' : 'ok';
    )");
    REQUIRE(result == "ok");
}

// ── MessageChannel + MessagePort + postMessage ─────────────────────────

TEST_CASE("MessageChannel constructs, exposes port1/port2, and postMessage doesn't throw",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var c = new MessageChannel();
        var ok = (typeof c.port1 === 'object' && typeof c.port2 === 'object'
                  && typeof c.port1.postMessage === 'function');
        c.port1.onmessage = function () {};
        c.port2.onmessage = function () {};
        c.port1.postMessage({k:'v'});  // queued microtask, no throw
        c.port1.start();
        c.port1.close();
        return ok ? 'ok' : 'broken';
    )");
    REQUIRE(result == "ok");
}

// Codex P2 on PR #730: React 18's scheduler specifically checks
// `window.postMessage` (not just `globalThis.postMessage`). `window` is a
// distinct object in this runtime, so the scheduler shim needs to mirror.
TEST_CASE("postMessage is available on both globalThis AND window",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        return [
            'globalThis=' + (typeof globalThis.postMessage),
            'window=' + (typeof window.postMessage),
            'same=' + (globalThis.postMessage === window.postMessage)
        ].join('|');
    )");
    REQUIRE(result == "globalThis=function|window=function|same=true");
}

// ── URLSearchParams ────────────────────────────────────────────────────

TEST_CASE("URLSearchParams parses, mutates, and serialises round-trip",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var p = new URLSearchParams('a=1&b=2&a=3');
        p.append('c', 'hi there');
        p.set('a', 'one');
        var out = [];
        out.push(p.get('a'));         // 'one' (set collapses dupes)
        out.push(p.get('b'));
        out.push(p.getAll('a').length); // 1 after set
        out.push(p.has('c'));
        out.push(p.toString());        // a=one&b=2&c=hi+there
        return out.join('|');
    )");
    REQUIRE(result == "one|2|1|true|a=one&b=2&c=hi+there");
}

TEST_CASE("URLSearchParams accepts a record-style object init",
          "[view][web-compat][issue-468]") {
    auto result = run_in_bridge(R"(
        var p = new URLSearchParams({a: 1, b: 'two'});
        return p.toString();
    )");
    // Object iteration order isn't guaranteed across engines; accept either.
    REQUIRE((result == "a=1&b=two" || result == "b=two&a=1"));
}
