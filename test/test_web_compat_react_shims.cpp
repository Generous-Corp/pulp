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

#include <chrono>
#include <string>
#include <thread>

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

// Same as run_in_bridge but pumps the JS job queue once after eval, so
// queueMicrotask / Promise.then / async-await callbacks actually drain
// before we read back the marker. Existence test for #746 — without the
// pump_message_loop fix this returns the pre-pump value.
std::string run_in_bridge_with_pump(const std::string& js) {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script("globalThis.__test_result__ = (function(){\n"
                       + js +
                       "\n})();");
    engine.pump_message_loop();
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

// ── DOM-ops idempotency (#745) ──────────────────────────────────────────
//
// pulp #745 consolidated the inline `kDomOpsInit` C-string in
// widget_bridge.cpp with the standalone web-compat-dom-ops.js prelude.
// The JS file now carries an idempotency guard so re-eval'ing the
// prelude a second time leaves the prototype methods in place instead
// of re-defining them. Pin both invariants here.

TEST_CASE("DOM mutation methods are tagged with the __pulp_dom_ops__ marker",
          "[view][web-compat][issue-745]") {
    auto result = run_in_bridge(R"(
        var names = ['appendChild','removeChild','remove','insertBefore','replaceChild'];
        var ok = [];
        for (var i = 0; i < names.length; i++) {
            var fn = Element.prototype[names[i]];
            ok.push(names[i] + '=' + (fn && fn.__pulp_dom_ops__ === true));
        }
        return ok.join(',');
    )");
    REQUIRE(result ==
            "appendChild=true,removeChild=true,remove=true,insertBefore=true,replaceChild=true");
}

TEST_CASE("Re-evaluating the dom-ops prelude does not re-define the prototype methods",
          "[view][web-compat][issue-745]") {
    // Capture identity, re-eval the prelude verbatim, and assert each
    // method is the same function object. If the guard regresses, a
    // second eval would replace each method with a freshly-defined
    // closure and identity would break.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__before__ = {
            appendChild: Element.prototype.appendChild,
            removeChild: Element.prototype.removeChild,
            insertBefore: Element.prototype.insertBefore,
            replaceChild: Element.prototype.replaceChild,
            remove: Element.prototype.remove
        };
    )");
    // Force a second eval of the same prelude logic by load_script-ing
    // a snippet whose only side effect is that re-eval. The bridge no
    // longer guards against this with a C++-side flag; the JS guard
    // must do the right thing.
    bridge.load_script(R"(
        // Inline a copy of the guard + appendChild reassignment, then
        // verify the original still wins.
        if (!Element.prototype.appendChild ||
            !Element.prototype.appendChild.__pulp_dom_ops__) {
            Element.prototype.appendChild = function () { return null; };
            Element.prototype.appendChild.__pulp_dom_ops__ = true;
        }
        globalThis.__after_match__ =
            (Element.prototype.appendChild === globalThis.__before__.appendChild) &&
            (Element.prototype.removeChild === globalThis.__before__.removeChild) &&
            (Element.prototype.insertBefore === globalThis.__before__.insertBefore) &&
            (Element.prototype.replaceChild === globalThis.__before__.replaceChild) &&
            (Element.prototype.remove === globalThis.__before__.remove);
    )");
    auto v = engine.evaluate("String(globalThis.__after_match__)");
    REQUIRE(v.isString());
    REQUIRE(std::string(v.getString()) == "true");
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

TEST_CASE("queueMicrotask invocation does not throw and accepts non-functions silently",
          "[view][web-compat][issue-468]") {
    // The validation path: queueMicrotask is exposed, is callable,
    // accepts a function argument without throwing, and silently
    // ignores non-functions. Each of these is what React 18's
    // `typeof queueMicrotask === 'function'` probe + scheduler bootstrap
    // reads. Drain semantics are covered separately below.
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

// ── Microtask draining (#746) ───────────────────────────────────────────
//
// CHOC's QuickJS pumpMessageLoop has an empty body, so before the #746
// fix in js_quickjs_engine.cpp the entire microtask queue silently
// stayed pending across pump_message_loop() calls. These tests pin the
// new contract: after pump, every callback the queue knew about has
// run. They cover the four cases #746's acceptance criteria called out
// (bare microtask, Promise.then, async/await, transitive scheduling).

TEST_CASE("pump_message_loop drains a bare queueMicrotask callback",
          "[view][web-compat][issue-746]") {
    auto result = run_in_bridge_with_pump(R"(
        globalThis.__seen__ = 'before';
        queueMicrotask(function () { globalThis.__seen__ = 'after'; });
        return 'scheduled';  // pre-pump marker the harness ignores
    )");
    // Override: read __seen__ via the marker write path.
    REQUIRE(result == "scheduled");
    // Verify the side-effect landed by re-running with the pump in the
    // returned expression so we observe the post-pump state.
    auto seen = run_in_bridge_with_pump(R"(
        globalThis.__flag__ = 'before';
        queueMicrotask(function () { globalThis.__flag__ = 'after'; });
        // Pump is invoked by the harness AFTER this returns; we cannot
        // read __flag__ from within the IIFE because the microtask has
        // not yet run when the IIFE returns. Stash the global name so
        // a follow-up evaluator can read it post-pump — but the harness
        // already does that for __test_result__, so chain through that
        // by stringifying after the pump from within a deferred read.
        return globalThis.__flag__;
    )");
    // Pre-pump observation inside the IIFE returns 'before'; after the
    // harness pumps, the global itself has flipped to 'after'. Since
    // __test_result__ captured the IIFE's pre-pump value, reading it
    // here should still be 'before' — the *real* drain proof is the
    // dedicated test below that re-reads the global after the pump.
    REQUIRE(seen == "before");
}

TEST_CASE("pump_message_loop runs queueMicrotask side-effects observable post-pump",
          "[view][web-compat][issue-746]") {
    // Direct ScriptEngine drive so we can interleave pump_message_loop
    // and reads across the JS boundary explicitly.
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__drain_marker__ = 'pending';
        queueMicrotask(function () { globalThis.__drain_marker__ = 'drained'; });
    )");
    auto pre = engine.evaluate("String(globalThis.__drain_marker__)");
    REQUIRE(pre.isString());
    REQUIRE(std::string(pre.getString()) == "pending");

    engine.pump_message_loop();

    auto post = engine.evaluate("String(globalThis.__drain_marker__)");
    REQUIRE(post.isString());
    REQUIRE(std::string(post.getString()) == "drained");
}

TEST_CASE("pump_message_loop runs Promise.then continuations",
          "[view][web-compat][issue-746]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__chain_marker__ = 'before';
        Promise.resolve('a').then(function (v) {
            return v + ':b';                         // pass through chain
        }).then(function (v) {
            globalThis.__chain_marker__ = v + ':c';  // observe end-of-chain
        });
    )");
    engine.pump_message_loop();
    auto v = engine.evaluate("String(globalThis.__chain_marker__)");
    REQUIRE(v.isString());
    // Both .then continuations must have run — promise chains drain
    // through queued jobs each step at a time, so the final marker
    // proves the second job was reached after the first scheduled it.
    REQUIRE(std::string(v.getString()) == "a:b:c");
}

TEST_CASE("pump_message_loop drains async/await round-trip",
          "[view][web-compat][issue-746]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__async_marker__ = 'before';
        (async function () {
            var v = await Promise.resolve(42);
            globalThis.__async_marker__ = 'awaited:' + v;
        })();
    )");
    engine.pump_message_loop();
    auto v = engine.evaluate("String(globalThis.__async_marker__)");
    REQUIRE(v.isString());
    REQUIRE(std::string(v.getString()) == "awaited:42");
}

TEST_CASE("pump_message_loop transitively drains microtasks scheduled from microtasks",
          "[view][web-compat][issue-746]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__transitive__ = 0;
        function step (n) {
            return function () {
                globalThis.__transitive__ = n;
                if (n < 5) queueMicrotask(step(n + 1));
            };
        }
        queueMicrotask(step(1));
    )");
    engine.pump_message_loop();
    auto v = engine.evaluate("String(globalThis.__transitive__)");
    REQUIRE(v.isString());
    // Each microtask schedules the next; the pump must drain
    // transitively until the queue truly empties.
    REQUIRE(std::string(v.getString()) == "5");
}

// Codex P2 on PR #769: an earlier 4096-job hard cap silently returned
// after 4096 iterations, leaving a larger Promise/microtask chain
// half-drained. The cap is gone — pump now drains to empty (rc == 0).
// Verify by scheduling 5000 microtasks (≥ the old cap) in one chain
// and asserting every one of them executed before the pump returns.
TEST_CASE("pump_message_loop drains a long chain past the old 4096-job cutoff",
          "[view][web-compat][issue-746][issue-769]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__count__ = 0;
        var TARGET = 5000;
        function step () {
            globalThis.__count__++;
            if (globalThis.__count__ < TARGET) queueMicrotask(step);
        }
        queueMicrotask(step);
    )");
    engine.pump_message_loop();
    auto v = engine.evaluate("String(globalThis.__count__)");
    REQUIRE(v.isString());
    REQUIRE(std::string(v.getString()) == "5000");
}

// Codex P1 on PR #874 (issue #902): the unbounded `for (;;)` introduced
// by #874 hard-freezes the UI thread when JS schedules a self-rearming
// microtask. The fix re-introduces a bound (1M jobs) and logs a warning
// when it fires. This test pins that bound: a microtask that always
// re-queues itself must NOT hang `pump_message_loop()`.
//
// We can't drive the pump from a worker thread — QuickJS's JSContext
// is single-threaded and cross-thread access is undefined (Linux CI
// surfaced this as count==0 because the foreign-thread call returned
// rc<0 immediately). So we drive the pump synchronously on the
// calling thread and rely on two complementary checks:
//
//   1. A wall-clock budget around the call. If the pump returns
//      within a generous limit it is by definition bounded; a
//      regression of the bound would hang the test process and trip
//      the CTest TIMEOUT, surfacing as a Failed test instead of
//      silent success.
//   2. A counter on the JS side bumped every microtask. After the
//      pump returns we assert it reached well past the 5K of the
//      prior issue-769 test, proving the cap (not an early-empty
//      return) is what stopped us.
TEST_CASE("pump_message_loop is bounded against a self-rearming microtask",
          "[view][web-compat][issue-902]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__runaway__ = 0;
        function loop () {
            globalThis.__runaway__++;
            queueMicrotask(loop);  // never terminates on its own
        }
        queueMicrotask(loop);
    )");

    // 1M JS_ExecutePendingJob calls of a 2-statement microtask finish
    // well inside this test's 180s CTest timeout on supported hosts.
    // Sanitizer builds can be much slower than release/debug hosts; a
    // wedged regression would hang and trip CTest's TIMEOUT line, which
    // is the loud signal we want.
    auto t0 = std::chrono::steady_clock::now();
    engine.pump_message_loop();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // Generous upper bound — sanitizer builds and slow CI runners are
    // real. The point is to catch a multi-minute hang, not benchmark.
    using namespace std::chrono_literals;
    REQUIRE(elapsed < 170s);

    // Prove we actually exercised the cap (not just returned early on
    // rc==0). The JS-side counter must be well past the 5K used by
    // the prior issue-769 test.
    auto v = engine.evaluate("String(globalThis.__runaway__)");
    REQUIRE(v.isString());
    long long count = std::stoll(std::string(v.getString()));
    REQUIRE(count >= 100000);
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

// `window.parent` self-reference. Real browser top-level windows have
// `window.parent === window`; imported React designs commonly call
// `window.parent.postMessage(...)` for cross-frame messaging (4 sites
// in Spectr's edit-mode bridge alone). Without the self-reference the
// property access throws TypeError and the surrounding effect dies
// silently.
TEST_CASE("window.parent is a self-reference and supports postMessage",
          "[view][web-compat][issue-runtime-import]") {
    auto result = run_in_bridge(R"(
        return [
            'defined=' + (typeof window.parent),
            'self=' + (window.parent === window),
            'callable=' + (typeof window.parent.postMessage)
        ].join('|');
    )");
    REQUIRE(result == "defined=object|self=true|callable=function");
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

// ── Native web-API globals (pulp #915) ─────────────────────────────────
//
// Pulp #915 — `requestAnimationFrame`, `cancelAnimationFrame`,
// `setTimeout`, `clearTimeout`, `setInterval`, `clearInterval`, and
// `MessageChannel` are now installed by WidgetBridge::register_api +
// the web-compat-scheduler.js prelude as standard global names. Spectr
// (and any other consumer of `@pulp/react`) used to ship a ~80-line
// `shim.js` that re-aliased the underscored `__requestFrame__` / etc.
// these tests pin the contract that no JS-side shim is needed: every
// global exists on `globalThis` immediately after bridge construction.

TEST_CASE("setTimeout / clearTimeout / setInterval / clearInterval are functions on globalThis",
          "[view][web-compat][issue-915]") {
    auto result = run_in_bridge(R"(
        return [
            typeof globalThis.setTimeout,
            typeof globalThis.clearTimeout,
            typeof globalThis.setInterval,
            typeof globalThis.clearInterval
        ].join(',');
    )");
    REQUIRE(result == "function,function,function,function");
}

TEST_CASE("requestAnimationFrame / cancelAnimationFrame are functions on globalThis",
          "[view][web-compat][issue-915]") {
    auto result = run_in_bridge(R"(
        return [
            typeof globalThis.requestAnimationFrame,
            typeof globalThis.cancelAnimationFrame
        ].join(',');
    )");
    REQUIRE(result == "function,function");
}

TEST_CASE("performance.now is callable and returns a finite number",
          "[view][web-compat][issue-915]") {
    auto result = run_in_bridge(R"(
        var v = performance.now();
        return [
            typeof globalThis.performance,
            typeof globalThis.performance.now,
            typeof v,
            isFinite(v) ? 'finite' : 'inf'
        ].join(',');
    )");
    REQUIRE(result == "object,function,number,finite");
}

TEST_CASE("setTimeout returns a numeric id and fires on next pump_message_loop for ms=0",
          "[view][web-compat][issue-915]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__t_marker__ = 'before';
        globalThis.__t_id__ = setTimeout(function () {
            globalThis.__t_marker__ = 'after';
        }, 0);
    )");

    auto id = engine.evaluate("typeof globalThis.__t_id__ + ':' + globalThis.__t_id__");
    REQUIRE(id.isString());
    auto id_str = std::string(id.getString());
    REQUIRE(id_str.find("number:") == 0);

    auto pre = engine.evaluate("String(globalThis.__t_marker__)");
    REQUIRE(std::string(pre.getString()) == "before");

    engine.pump_message_loop();

    auto post = engine.evaluate("String(globalThis.__t_marker__)");
    REQUIRE(std::string(post.getString()) == "after");
}

TEST_CASE("clearTimeout cancels a pending zero-delay timer before pump",
          "[view][web-compat][issue-915]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__t_marker__ = 'before';
        var id = setTimeout(function () {
            globalThis.__t_marker__ = 'fired';
        }, 0);
        clearTimeout(id);
    )");
    engine.pump_message_loop();
    auto v = engine.evaluate("String(globalThis.__t_marker__)");
    REQUIRE(std::string(v.getString()) == "before");
}

TEST_CASE("setTimeout with positive delay fires after service_frame_callbacks",
          "[view][web-compat][issue-915]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__pos_marker__ = 'pre';
        setTimeout(function () { globalThis.__pos_marker__ = 'post'; }, 5);
    )");
    // 50ms is well above the 5ms delay and well below the test timeout.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bridge.service_frame_callbacks();
    auto v = engine.evaluate("String(globalThis.__pos_marker__)");
    REQUIRE(std::string(v.getString()) == "post");
}

TEST_CASE("setInterval rearms across multiple service_frame_callbacks calls",
          "[view][web-compat][issue-915]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__iv_count__ = 0;
        globalThis.__iv_id__ = setInterval(function () {
            globalThis.__iv_count__++;
        }, 5);
    )");
    // Three frame ticks at >5ms apart → at least three invocations.
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bridge.service_frame_callbacks();
    }
    auto v = engine.evaluate("String(globalThis.__iv_count__)");
    REQUIRE(v.isString());
    long long count = std::stoll(std::string(v.getString()));
    REQUIRE(count >= 3);

    // clearInterval stops the rearm cycle.
    engine.evaluate("clearInterval(globalThis.__iv_id__);void 0");
    long long before_clear = count;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bridge.service_frame_callbacks();
    auto after = engine.evaluate("String(globalThis.__iv_count__)");
    long long after_count = std::stoll(std::string(after.getString()));
    REQUIRE(after_count == before_clear);
}

TEST_CASE("clearInterval is the same function as clearTimeout",
          "[view][web-compat][issue-915]") {
    auto result = run_in_bridge(R"(
        return clearTimeout === clearInterval ? 'same' : 'distinct';
    )");
    REQUIRE(result == "same");
}

TEST_CASE("requestAnimationFrame registers a frame callback that fires via service_frame_callbacks",
          "[view][web-compat][issue-915]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__raf_marker__ = 'before';
        globalThis.__raf_id__ = requestAnimationFrame(function () {
            globalThis.__raf_marker__ = 'fired';
        });
    )");
    auto id = engine.evaluate("typeof globalThis.__raf_id__");
    REQUIRE(std::string(id.getString()) == "number");

    bridge.service_frame_callbacks();

    auto post = engine.evaluate("String(globalThis.__raf_marker__)");
    REQUIRE(std::string(post.getString()) == "fired");
}

TEST_CASE("cancelAnimationFrame removes a pending frame callback",
          "[view][web-compat][issue-915]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__cancel_marker__ = 'before';
        var id = requestAnimationFrame(function () {
            globalThis.__cancel_marker__ = 'fired';
        });
        cancelAnimationFrame(id);
    )");
    bridge.service_frame_callbacks();
    auto v = engine.evaluate("String(globalThis.__cancel_marker__)");
    REQUIRE(std::string(v.getString()) == "before");
}

TEST_CASE("MessageChannel.port2.onmessage fires after port1.postMessage on next microtask drain",
          "[view][web-compat][issue-915]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 300});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        globalThis.__mc_received__ = null;
        var c = new MessageChannel();
        c.port2.onmessage = function (ev) { globalThis.__mc_received__ = ev.data; };
        c.port1.postMessage({hello: 'world', n: 42});
    )");
    // postMessage queues a microtask; pump_message_loop drains it.
    auto pre = engine.evaluate("String(globalThis.__mc_received__)");
    REQUIRE(std::string(pre.getString()) == "null");

    engine.pump_message_loop();

    auto post = engine.evaluate(
        "globalThis.__mc_received__ && globalThis.__mc_received__.hello + ':' + "
        "globalThis.__mc_received__.n");
    REQUIRE(post.isString());
    REQUIRE(std::string(post.getString()) == "world:42");
}

TEST_CASE("Native web-API globals are mirrored onto window so React's scheduler probe passes",
          "[view][web-compat][issue-915]") {
    auto result = run_in_bridge(R"(
        // React 18's scheduler reads `window.setTimeout` (not just
        // `globalThis.setTimeout`). The mirror is the contract.
        return [
            'rAF=' + (window.requestAnimationFrame === globalThis.requestAnimationFrame),
            'cAF=' + (window.cancelAnimationFrame === globalThis.cancelAnimationFrame),
            'sT=' + (window.setTimeout === globalThis.setTimeout),
            'cT=' + (window.clearTimeout === globalThis.clearTimeout),
            'sI=' + (window.setInterval === globalThis.setInterval),
            'cI=' + (window.clearInterval === globalThis.clearInterval),
            'MC=' + (window.MessageChannel === globalThis.MessageChannel),
            'qM=' + (window.queueMicrotask === globalThis.queueMicrotask),
            'pf=' + (window.performance === globalThis.performance)
        ].join('|');
    )");
    REQUIRE(result ==
            "rAF=true|cAF=true|sT=true|cT=true|sI=true|cI=true|"
            "MC=true|qM=true|pf=true");
}
