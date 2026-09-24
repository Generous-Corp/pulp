// Keyboard navigation for a menu no trigger owns.
//
// The web-compat popup owner already gives dropdowns a roving cursor:
// ArrowUp/ArrowDown step `activeIndex`, the stepped row is marked
// `data-pulp-popup-active`, Home/End jump, Enter activates and Escape
// dismisses. All of it was reachable only through a TRIGGER, because the
// state machine is entered from `triggerFrom(document.activeElement)` and
// every later branch returns early on `!state`.
//
// A context menu has no trigger. It is summoned at coordinates by a pointer
// press, so no `aria-haspopup` element owns it, no state is ever built, and
// arrow keys did nothing inside one — while the header dropdowns in the same
// document navigated correctly. Menu items also carry no `tabIndex`, so an
// app's own per-item key handlers could never be reached either: without a
// cursor there is no way to put keyboard focus on a row.
//
// An open `role="menu"` is now ADOPTED on the first arrow, through the same
// state machine, so navigation is framework behavior every Pulp menu inherits
// rather than something each app re-implements.
//
// The negative controls are the point of this file: adoption must not fire
// when nothing is open (the key has to stay available to the app and the DAW),
// must not capture a menu a trigger already owns (dropdowns keep their exact
// behavior), and must not guess between two open menus.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <string>

using namespace pulp::view;
using namespace pulp::state;

namespace {

struct Harness {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge;

    Harness() : root(), store(), bridge(engine, root, store) {
        root.set_bounds({0, 0, 400, 300});
    }
    void eval(const std::string& js) { bridge.load_script(js); }
    std::string read(const std::string& expr) {
        return engine.evaluate(expr).toString();
    }
    void key(KeyCode code) {
        bridge.forward_key_event(static_cast<int>(code), 0, true);
    }
};

// A trigger-less `role="menu"` with three rows, shaped like Spectr's band
// context menu: rows are <button role="menuitem"> with no tabIndex.
constexpr const char* kUnownedMenu = R"(
    var menu = document.createElement('div');
    menu.setAttribute('role', 'menu');
    document.body.appendChild(menu);
    var rows = [];
    for (var i = 0; i < 3; ++i) {
        var b = document.createElement('button');
        b.setAttribute('role', 'menuitem');
        b.textContent = 'row' + i;
        menu.appendChild(b);
        rows.push(b);
    }
    // Returned as STRINGS on purpose: ScriptEngine::Value::toString() yields
    // "" for a number, so a numeric helper would make every assertion compare
    // "" against its expectation and fail identically whatever the code did.
    // PRESENCE of data-pulp-popup-active means Pulp owns the popup and is set
    // on every row; the VALUE "true" is the painted cursor and is set on one.
    // Counting presence would report every row active and always find row 0.
    function cursorOn(el) {
        return el.getAttribute('data-pulp-popup-active') === 'true';
    }
    function activeRows() {
        var n = 0;
        for (var i = 0; i < rows.length; ++i) if (cursorOn(rows[i])) n++;
        return '' + n;
    }
    function activeAt() {
        for (var i = 0; i < rows.length; ++i) if (cursorOn(rows[i])) return '' + i;
        return '-1';
    }
)";

}  // namespace

// ── positive: an unowned menu gets a cursor ─────────────────────────────

TEST_CASE("menu keynav: ArrowDown gives a trigger-less role=menu a cursor",
          "[view][web-compat][menu-keynav]") {
    Harness h;
    h.eval(kUnownedMenu);
    REQUIRE(h.read("activeRows()") == "0");  // nothing marked before any key
    h.key(KeyCode::down);
    REQUIRE(h.read("activeRows()") == "1");
    REQUIRE(h.read("activeAt()") == "0");
}

TEST_CASE("menu keynav: ArrowUp enters a trigger-less menu at the last row",
          "[view][web-compat][menu-keynav]") {
    Harness h;
    h.eval(kUnownedMenu);
    h.key(KeyCode::up);
    REQUIRE(h.read("activeAt()") == "2");
}

TEST_CASE("menu keynav: ArrowDown steps exactly one row per press",
          "[view][web-compat][menu-keynav]") {
    Harness h;
    h.eval(kUnownedMenu);
    h.key(KeyCode::down);
    h.key(KeyCode::down);
    REQUIRE(h.read("activeAt()") == "1");
    REQUIRE(h.read("activeRows()") == "1");  // never two cursors at once
}

// ── negative control: nothing open ──────────────────────────────────────
//
// The load-bearing control. If adoption fired with no menu open it would
// swallow every arrow key in the editor, and the host forwards unconsumed
// keys to the DAW — so a hijacked ArrowDown is a key the DAW never sees.

TEST_CASE("menu keynav: with no menu open, an arrow marks nothing",
          "[view][web-compat][menu-keynav][control]") {
    Harness h;
    h.eval(R"(
        var seen = [];
        window.addEventListener('keydown', function(e) {
            seen.push(e.key + ':' + (e.defaultPrevented ? 'taken' : 'free'));
        });
    )");
    h.key(KeyCode::down);
    // The app still receives it, and nothing claimed it.
    REQUIRE(h.read("seen.join(',')") == "ArrowDown:free");
}

// ── negative control: a menu a trigger already owns ─────────────────────

TEST_CASE("menu keynav: a trigger-owned menu is left to the trigger path",
          "[view][web-compat][menu-keynav][control]") {
    Harness h;
    h.eval(R"(
        var trig = document.createElement('button');
        trig.setAttribute('aria-haspopup', 'menu');
        trig.setAttribute('aria-controls', 'owned');
        document.body.appendChild(trig);
        var menu = document.createElement('div');
        menu.id = 'owned';
        menu.setAttribute('role', 'menu');
        document.body.appendChild(menu);
        var b = document.createElement('button');
        b.setAttribute('role', 'menuitem');
        menu.appendChild(b);
        function ownedActive() {
            return b.getAttribute('data-pulp-popup-active') === 'true'
                ? 'marked' : 'clean';
        }
    )");
    // Nothing has focus, so the trigger path cannot fire either; the point is
    // that the UNOWNED path must not step in and mark the row behind its back.
    h.key(KeyCode::down);
    REQUIRE(h.read("ownedActive()") == "clean");
}

// ── negative control: ambiguity ─────────────────────────────────────────

TEST_CASE("menu keynav: two open unowned menus are not guessed between",
          "[view][web-compat][menu-keynav][control]") {
    Harness h;
    h.eval(kUnownedMenu);
    h.eval(R"(
        var second = document.createElement('div');
        second.setAttribute('role', 'menu');
        document.body.appendChild(second);
        var b2 = document.createElement('button');
        b2.setAttribute('role', 'menuitem');
        second.appendChild(b2);
    )");
    h.key(KeyCode::down);
    REQUIRE(h.read("activeRows()") == "0");  // refused rather than guessed
}

// ── negative control: an empty menu cannot swallow keys ─────────────────

TEST_CASE("menu keynav: a menu with no rows is not adopted",
          "[view][web-compat][menu-keynav][control]") {
    Harness h;
    h.eval(R"(
        var empty = document.createElement('div');
        empty.setAttribute('role', 'menu');
        document.body.appendChild(empty);
        var seen = [];
        window.addEventListener('keydown', function(e) {
            seen.push(e.defaultPrevented ? 'taken' : 'free');
        });
    )");
    h.key(KeyCode::down);
    REQUIRE(h.read("seen.join(',')") == "free");
}

// ── opt-out is honored ──────────────────────────────────────────────────

TEST_CASE("menu keynav: data-pulp-popup-default=off opts a menu out",
          "[view][web-compat][menu-keynav][control]") {
    Harness h;
    h.eval(kUnownedMenu);
    h.eval("menu.setAttribute('data-pulp-popup-default', 'off');");
    h.key(KeyCode::down);
    REQUIRE(h.read("activeRows()") == "0");
}
