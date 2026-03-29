// Web-compat prelude tests — validates the JS prelude layer
// Tests CSS parsing, named colors, document API, classList, querySelector.
// Note: appendChild tests are excluded pending QuickJS stack size fix
// for deep JS↔C++ interleaving in _reparentNative.

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include <pulp/view/ui_components.hpp>
#include <pulp/view/canvas_widget.hpp>

using namespace pulp::test;
using namespace pulp::view;

// ═══════════════════════════════════════════════════════════════════════════════
// Prelude loaded checks
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: named colors available", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("__cssColors__['red']");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff0000");
}

TEST_CASE("WebCompat: named colors cornflowerblue", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("__cssColors__['cornflowerblue']");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#6495ed");
}

TEST_CASE("WebCompat: document object exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("typeof document");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "object");
}

TEST_CASE("WebCompat: document.body exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.body !== null && document.body !== undefined");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: Element constructor exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("typeof Element");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "function");
}

TEST_CASE("WebCompat: StyleSheet constructor exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("typeof StyleSheet");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "function");
}

// ═══════════════════════════════════════════════════════════════════════════════
// CSS parser functions
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: parseCSSColor red", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('red')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff0000");
}

TEST_CASE("WebCompat: parseCSSColor hex", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('#1e90ff')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#1e90ff");
}

TEST_CASE("WebCompat: parseCSSColor hex short", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('#f00')");
    // Short hex is preserved as-is by the parser (bridge expands it)
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#f00");
}

TEST_CASE("WebCompat: parseCSSColor rgb", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('rgb(255, 128, 0)')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff8000");
}

TEST_CASE("WebCompat: parseCSSColor hsl red", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('hsl(0, 100%, 50%)')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff0000");
}

TEST_CASE("WebCompat: parseCSSColor transparent", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('transparent')");
    // Parser resolves transparent to rgba hex
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#00000000");
}

TEST_CASE("WebCompat: parseCSSLength px", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('20px').value");
    REQUIRE(result.getWithDefault<double>(0.0) == 20.0);
}

TEST_CASE("WebCompat: parseCSSLength unit", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('20px').unit");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "px");
}

TEST_CASE("WebCompat: parseCSSLength percent", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('50%').unit");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "%");
}

TEST_CASE("WebCompat: parseCSSLength auto", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('auto').unit");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "auto");
}

TEST_CASE("WebCompat: parseCSSLength bare number", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('42').value");
    REQUIRE(result.getWithDefault<double>(0.0) == 42.0);
}

TEST_CASE("WebCompat: expandShorthand 1 value", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,10,10,10]");
}

TEST_CASE("WebCompat: expandShorthand 2 values", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px 20px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,20,10,20]");
}

TEST_CASE("WebCompat: expandShorthand 3 values", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px 20px 30px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,20,30,20]");
}

TEST_CASE("WebCompat: expandShorthand 4 values", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px 20px 30px 40px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,20,30,40]");
}

TEST_CASE("WebCompat: parseTransform scale", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransform('scale(1.5)')[0].fn");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "scale");
}

TEST_CASE("WebCompat: parseTransform multiple", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransform('scale(1.5) rotate(45)').length");
    REQUIRE(result.getWithDefault<int>(0) == 2);
}

TEST_CASE("WebCompat: parseTransition", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransition('opacity 300ms ease').property");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "opacity");
}

// ═══════════════════════════════════════════════════════════════════════════════
// createElement (without appendChild — just JS-side construction)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: createElement returns Element", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('div') instanceof Element");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: createElement tagName", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('span').tagName");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "SPAN");
}

TEST_CASE("WebCompat: element.id assignment", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.id = 'test123';");
    auto result = env.engine.evaluate("el.id");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "test123");
}

TEST_CASE("WebCompat: element.textContent", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('span'); el.textContent = 'Hello World';");
    auto result = env.engine.evaluate("el.textContent");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "Hello World");
}

TEST_CASE("WebCompat: element.hidden default false", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('div').hidden");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: element.disabled default false", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('div').disabled");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// classList (JS-side only, no native calls)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: classList.add", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.add('active');");
    auto result = env.engine.evaluate("el.classList.contains('active')");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: classList.remove", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.add('active'); el.classList.remove('active');");
    auto result = env.engine.evaluate("el.classList.contains('active')");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: classList.toggle on", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.toggle('on');");
    auto result = env.engine.evaluate("el.classList.contains('on')");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: classList.toggle off", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.add('on'); el.classList.toggle('on');");
    auto result = env.engine.evaluate("el.classList.contains('on')");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: className setter", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.className = 'foo bar';");
    auto r1 = env.engine.evaluate("el.classList.contains('foo')");
    auto r2 = env.engine.evaluate("el.classList.contains('bar')");
    REQUIRE(r1.getWithDefault<bool>(false) == true);
    REQUIRE(r2.getWithDefault<bool>(false) == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// StyleSheet (JS-side construction)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: StyleSheet construction", "[webcompat][stylesheet]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("new StyleSheet({'.box': {width: '100px'}}) instanceof StyleSheet");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// CSSStyleDeclaration (JS-side, pending styles before appendChild)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: style property stores pending value", "[webcompat][style]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.style.width = '200px';");
    // Before appendChild, the style is pending — verify the _pending map has it
    auto result = env.engine.evaluate("el.style._props['width']");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "200px");
}

TEST_CASE("WebCompat: setAttribute/getAttribute", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.setAttribute('data-name', 'test');");
    auto result = env.engine.evaluate("el.getAttribute('data-name')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "test");
}

TEST_CASE("WebCompat: dataset from data attribute", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.setAttribute('data-user-id', '42');");
    auto result = env.engine.evaluate("el.dataset.userId");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "42");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Debug: direct createCol works after prelude
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: direct createCol still works", "[webcompat][native]") {
    TestEnvironment env;
    env.eval("createCol('directtest');");
    REQUIRE(env.widget("directtest") != nullptr);
}

TEST_CASE("WebCompat: manual _reparentNative works", "[webcompat][native]") {
    TestEnvironment env;
    env.eval(R"JS(
        var d = document.createElement('div');
        d.id = 'manual1';
        _reparentNative(d, '__body__');
    )JS");
    // Widget is registered under internal _id, not user .id
    auto result = env.engine.evaluate("d._nativeCreated");
    REQUIRE(result.getWithDefault<bool>(false) == true);
    // Also verify the widget exists in the bridge by internal id
    auto idResult = env.engine.evaluate("d._id");
    auto internalId = std::string(idResult.getWithDefault<std::string_view>(""));
    REQUIRE(env.widget(internalId) != nullptr);
}

TEST_CASE("WebCompat: createCol under root", "[webcompat][native]") {
    TestEnvironment env;
    env.eval("createCol('rootchild', '__root__');");
    REQUIRE(env.widget("rootchild") != nullptr);
}

TEST_CASE("WebCompat: manual appendChild works", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __testD = document.createElement('div');
        __testD._parentElement = document.body;
        document.body._children.push(__testD);
        _reparentNative(__testD, document.body._id);
        var __testResult = __testD._nativeCreated;
        var __testInternalId = __testD._id;
    )JS");
    auto r = env.engine.evaluate("__testResult");
    REQUIRE(r.getWithDefault<bool>(false) == true);
    auto id = std::string(env.engine.evaluate("__testInternalId").getWithDefault<std::string_view>(""));
    REQUIRE(env.widget(id) != nullptr);
}

// Note: document.body.appendChild() crashes due to QuickJS stack overflow
// when the method is dispatched through Element.prototype chain after the
// large prelude has consumed most of the 256KB JS stack. The manual
// _reparentNative approach works (see test above). Fix requires either:
// (a) Increasing JS_DEFAULT_STACK_SIZE in CHOC's QuickJS, or
// (b) Further splitting the web-compat prelude to reduce stack usage.
