// Selector/ID matching tests — validates widget lookup by ID and type checking

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;

TEST_CASE("Selector: widget lookup by ID", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(
        createPanel("header");
        createPanel("content");
        createPanel("footer");
    )JS");

    REQUIRE(env.widget("header") != nullptr);
    REQUIRE(env.widget("content") != nullptr);
    REQUIRE(env.widget("footer") != nullptr);
    REQUIRE(env.widget("nonexistent") == nullptr);
}

TEST_CASE("Selector: createKnob creates Knob type", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(createKnob("k1");)JS");
    auto* w = env.widget("k1");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<Knob*>(w) != nullptr);
}

TEST_CASE("Selector: createFader creates Fader type", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(createFader("f1", "vertical");)JS");
    auto* w = env.widget("f1");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<Fader*>(w) != nullptr);
}

TEST_CASE("Selector: createToggle creates Toggle type", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(createToggle("t1");)JS");
    auto* w = env.widget("t1");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<Toggle*>(w) != nullptr);
}

TEST_CASE("Selector: createLabel creates Label type", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(createLabel("lbl");)JS");
    auto* w = env.widget("lbl");
    REQUIRE(w != nullptr);
    REQUIRE(dynamic_cast<Label*>(w) != nullptr);
}

TEST_CASE("Selector: widget ID matches set_id", "[events][selector]") {
    View v;
    v.set_id("my-widget");
    REQUIRE(v.id() == "my-widget");
}

TEST_CASE("Selector: ID empty by default", "[events][selector]") {
    View v;
    REQUIRE(v.id().empty());
}

TEST_CASE("Selector: removeWidget removes from lookup", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(
        createPanel("temp");
        removeWidget("temp");
    )JS");
    REQUIRE(env.widget("temp") == nullptr);
}

TEST_CASE("Selector: JS parser matches compound selectors", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __selectorEl = document.createElement("button");
        __selectorEl.id = "bypass";
        __selectorEl.className = "control primary armed";
        __selectorEl.setAttribute("data-role", "transport");
        __selectorEl.setAttribute("aria-label", "Bypass Switch");
    )JS");

    auto matched = env.engine.evaluate(R"JS(
        _matchesSelector(
            __selectorEl,
            _parseSelector("button#bypass.control.primary[data-role='transport'][aria-label^='Bypass']")
        )
    )JS");
    REQUIRE(matched.getWithDefault<bool>(false) == true);

    auto rejected = env.engine.evaluate(R"JS(
        _matchesSelector(__selectorEl, _parseSelector("button#bypass.control.muted"))
    )JS");
    REQUIRE(rejected.getWithDefault<bool>(true) == false);
}

TEST_CASE("Selector: JS parser distinguishes child and descendant combinators", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __selectorGrandparent = document.createElement("section");
        var __selectorParent = document.createElement("div");
        var __selectorChild = document.createElement("span");

        __selectorGrandparent.className = "rack";
        __selectorParent.className = "strip";
        __selectorChild.className = "value";

        __selectorGrandparent._children.push(__selectorParent);
        __selectorParent._parentElement = __selectorGrandparent;
        __selectorParent._children.push(__selectorChild);
        __selectorChild._parentElement = __selectorParent;
    )JS");

    auto descendant = env.engine.evaluate(R"JS(
        _matchesSelector(__selectorChild, _parseSelector("section.rack span.value"))
    )JS");
    REQUIRE(descendant.getWithDefault<bool>(false) == true);

    auto direct = env.engine.evaluate(R"JS(
        _matchesSelector(__selectorChild, _parseSelector("section.rack > span.value"))
    )JS");
    REQUIRE(direct.getWithDefault<bool>(true) == false);

    auto immediate = env.engine.evaluate(R"JS(
        _matchesSelector(__selectorChild, _parseSelector("div.strip > span.value"))
    )JS");
    REQUIRE(immediate.getWithDefault<bool>(false) == true);
}

TEST_CASE("Selector: JS querySelectorAll walks nested children", "[events][selector]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __selectorList = document.createElement("div");
        var __selectorA = document.createElement("span");
        var __selectorB = document.createElement("span");
        var __selectorC = document.createElement("span");

        __selectorA.className = "item";
        __selectorB.className = "item selected";
        __selectorC.className = "item";

        __selectorList._children = [__selectorA, __selectorB, __selectorC];
        __selectorA._parentElement = __selectorList;
        __selectorB._parentElement = __selectorList;
        __selectorC._parentElement = __selectorList;
    )JS");

    auto firstMatch = env.engine.evaluate(R"JS(
        _querySelector(__selectorList, "span.selected") === __selectorB
    )JS");
    REQUIRE(firstMatch.getWithDefault<bool>(false) == true);

    auto matchCount = env.engine.evaluate(R"JS(
        _querySelectorAll(__selectorList, "span.item").length
    )JS");
    REQUIRE(matchCount.getWithDefault<int32_t>(0) == 3);

    auto descendantCount = env.engine.evaluate(R"JS(
        _querySelectorAll(__selectorList, "div span.item").length
    )JS");
    REQUIRE(descendantCount.getWithDefault<int32_t>(0) == 3);
}

// pulp-internal Tier-1 closure for css/__selector_id (2026-05-12). The
// one-pager listed `[id='foo']` as an unsupported attribute-selector
// form. Code reading shows the existing _parseSelector + _matchesSelector
// attribute-selector path already handles `[attr=value]`, so by
// inspection `[id='foo']` should already work via that path. Pin the
// contract with a regression test so the catalog can move the row
// from unsupportedValues to supportedValues honestly.
TEST_CASE("Selector: [id='foo'] attribute form matches via _matchesSelector attrs path",
          "[events][selector][tier1-closure]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __idEl = document.createElement("div");
        __idEl.id = "target";

        var __noidEl = document.createElement("div");
        __noidEl.id = "other";
    )JS");

    // [id='target'] should match the element whose getAttribute('id')
    // returns "target". Single-quoted, double-quoted, and unquoted
    // forms are all valid CSS — exercise all three.
    auto sq = env.engine.evaluate(R"JS(
        _matchesSelector(__idEl, _parseSelector("[id='target']"))
    )JS");
    auto dq = env.engine.evaluate(R"JS(
        _matchesSelector(__idEl, _parseSelector("[id=\"target\"]"))
    )JS");
    auto uq = env.engine.evaluate(R"JS(
        _matchesSelector(__idEl, _parseSelector("[id=target]"))
    )JS");
    REQUIRE(sq.getWithDefault<bool>(false) == true);
    REQUIRE(dq.getWithDefault<bool>(false) == true);
    REQUIRE(uq.getWithDefault<bool>(false) == true);

    // Negative case — another element with a different id must NOT
    // match `[id='target']`. Pins that the attrs path actually
    // consults getAttribute('id') rather than always returning true.
    auto neg = env.engine.evaluate(R"JS(
        _matchesSelector(__noidEl, _parseSelector("[id='target']"))
    )JS");
    REQUIRE(neg.getWithDefault<bool>(true) == false);

    // Compound: `div[id='target']` — tag AND id-via-attribute. Pins
    // that the attrs path composes correctly with the tag path.
    auto compound = env.engine.evaluate(R"JS(
        _matchesSelector(__idEl, _parseSelector("div[id='target']"))
    )JS");
    REQUIRE(compound.getWithDefault<bool>(false) == true);
}

// pulp-internal Tier-1 closure for css/__selector_tag (2026-05-12). The
// one-pager listed `*` (universal selector) as not parsed. Verify whether
// it actually works by inspection of _parseSelector: tokenRe drops `*`
// silently, so parsed.tag stays null and _matchesSelector skips the tag
// check. Empirically this means `*` already matches everything as a
// side-effect; pin the contract so it's not accidental.
TEST_CASE("Selector: * universal selector matches any element (currently via null-tag fall-through)",
          "[events][selector][tier1-closure]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __uniDiv  = document.createElement("div");
        var __uniSpan = document.createElement("span");
        var __uniBtn  = document.createElement("button");
        __uniBtn.className = "primary";
    )JS");

    // Bare `*` matches every element (tag-agnostic, attribute-agnostic).
    auto a = env.engine.evaluate(R"JS(_matchesSelector(__uniDiv,  _parseSelector("*")))JS");
    auto b = env.engine.evaluate(R"JS(_matchesSelector(__uniSpan, _parseSelector("*")))JS");
    auto c = env.engine.evaluate(R"JS(_matchesSelector(__uniBtn,  _parseSelector("*")))JS");
    REQUIRE(a.getWithDefault<bool>(false) == true);
    REQUIRE(b.getWithDefault<bool>(false) == true);
    REQUIRE(c.getWithDefault<bool>(false) == true);

    // `*.primary` should match the .primary button but not the others
    // (tag-agnostic + class-restricted).
    auto starClass1 = env.engine.evaluate(R"JS(_matchesSelector(__uniBtn,  _parseSelector("*.primary")))JS");
    auto starClass2 = env.engine.evaluate(R"JS(_matchesSelector(__uniDiv,  _parseSelector("*.primary")))JS");
    REQUIRE(starClass1.getWithDefault<bool>(false) == true);
    REQUIRE(starClass2.getWithDefault<bool>(true) == false);
}

TEST_CASE("Selector: parent() returns correct parent", "[events][selector]") {
    View root;
    auto child = std::make_unique<View>();
    auto* cp = child.get();
    root.add_child(std::move(child));
    REQUIRE(cp->parent() == &root);
}

TEST_CASE("Selector: parent of root is null", "[events][selector]") {
    View root;
    REQUIRE(root.parent() == nullptr);
}

TEST_CASE("Selector: child_at returns correct child", "[events][selector]") {
    View root;
    auto a = std::make_unique<View>();
    auto* ap = a.get();
    auto b = std::make_unique<View>();
    auto* bp = b.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    REQUIRE(root.child_at(0) == ap);
    REQUIRE(root.child_at(1) == bp);
}

TEST_CASE("Selector: child_count tracks additions", "[events][selector]") {
    View root;
    REQUIRE(root.child_count() == 0);
    root.add_child(std::make_unique<View>());
    REQUIRE(root.child_count() == 1);
    root.add_child(std::make_unique<View>());
    REQUIRE(root.child_count() == 2);
}
