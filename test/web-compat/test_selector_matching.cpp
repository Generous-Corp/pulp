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
