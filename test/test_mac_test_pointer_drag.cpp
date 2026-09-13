// PULP_TEST_POINTER_DRAG spelling contract.
//
// The GPU window host reads this variable once at construction and injects the
// described gesture into AppKit from its display-link tick. Nothing downstream
// of the parse is unit-testable — it needs a real NSWindow, a real hit-test,
// and a real frame clock — so the spelling is where a mistake has to be caught.
//
// Two of the three spellings are load-bearing history: `1` and `minimap` anchor
// every interaction measurement taken before the rect form existed. A changed
// prefix rule, a changed step count, or a changed path would silently
// re-baseline them, so their acceptance rules are pinned here alongside the new
// form's.

#include <catch2/catch_test_macros.hpp>

#include "../core/view/platform/mac/window_host_mac_internal.hpp"

using pulp::view::mac_test_drag::Mode;
using pulp::view::mac_test_drag::parse_test_pointer_drag;
using pulp::view::mac_test_drag::Spec;

TEST_CASE("legacy pointer-drag spellings keep their exact matching rules",
          "[mac][pointer-drag]") {
    // Unset means unset: no drive, no default gesture.
    CHECK(parse_test_pointer_drag(nullptr).mode == Mode::disabled);
    CHECK(parse_test_pointer_drag("").mode == Mode::disabled);
    CHECK(parse_test_pointer_drag("0").mode == Mode::disabled);

    // `minimap` is an EXACT match; `1` is a leading-character match.
    CHECK(parse_test_pointer_drag("minimap").mode == Mode::minimap);
    CHECK(parse_test_pointer_drag("minimap2").mode == Mode::disabled);
    CHECK(parse_test_pointer_drag("1").mode == Mode::bands);
    CHECK(parse_test_pointer_drag("1 ").mode == Mode::bands);
    CHECK(parse_test_pointer_drag("1234").mode == Mode::bands);

    // Neither legacy mode carries geometry — the drive holds their paths.
    CHECK(parse_test_pointer_drag("minimap").samples == 0);
    CHECK(parse_test_pointer_drag("1").samples == 0);
}

TEST_CASE("rect: describes a drag in normalized top-left coordinates",
          "[mac][pointer-drag]") {
    const Spec spec = parse_test_pointer_drag("rect:0.25,0.5,0.75,0.5,120");
    REQUIRE(spec.mode == Mode::rect);
    CHECK(spec.x0 == 0.25);
    CHECK(spec.y0 == 0.5);
    CHECK(spec.x1 == 0.75);
    CHECK(spec.y1 == 0.5);
    CHECK(spec.samples == 120);
    CHECK(spec.repeats == 1);  // omitted ⇒ a single gesture

    const Spec repeated = parse_test_pointer_drag("rect:0,0,1,1,4,6");
    REQUIRE(repeated.mode == Mode::rect);
    CHECK(repeated.x0 == 0.0);
    CHECK(repeated.y1 == 1.0);
    CHECK(repeated.samples == 4);
    CHECK(repeated.repeats == 6);

    // Integers, exponents, and the unit endpoints are all ordinary values.
    CHECK(parse_test_pointer_drag("rect:0,0,1,1,1").mode == Mode::rect);
    CHECK(parse_test_pointer_drag("rect:5e-1,0.5,0.5,0.5,10").x0 == 0.5);
}

TEST_CASE("a malformed rect: drive is refused, never approximated",
          "[mac][pointer-drag]") {
    // A typo here would otherwise drag off-window and produce a plausible
    // idle trace with no input workload at all — the exact failure the
    // direct-injection drive exists to avoid.
    const char* rejected[] = {
        "rect:",                      // no fields
        "rect:0.1,0.2,0.3",           // too few
        "rect:0.1,0.2,0.3,0.4",       // no step count
        "rect:0.1,0.2,0.3,0.4,10,2,9",// too many
        "rect:0.1,0.2,0.3,0.4,10,",   // trailing comma
        "rect:0.1,,0.3,0.4,10",       // empty field
        "rect:0.5x,0.2,0.3,0.4,10",   // trailing garbage in a field
        "rect:abc,0.2,0.3,0.4,10",    // not a number
        "rect:-0.1,0.2,0.3,0.4,10",   // below the window
        "rect:0.1,1.5,0.3,0.4,10",    // past the window
        "rect:0.1,0.2,0.3,0.4,0",     // a gesture with no steps
        "rect:0.1,0.2,0.3,0.4,-5",    // negative steps
        "rect:0.1,0.2,0.3,0.4,10.5",  // fractional steps
        "rect:0.1,0.2,0.3,0.4,10,0",  // zero repeats
        "rect:0.1,0.2,0.3,0.4,999999",// past the step ceiling
        "rect 0.1,0.2,0.3,0.4,10",    // wrong separator
        "Rect:0.1,0.2,0.3,0.4,10",    // wrong case
    };
    for (const char* value : rejected) {
        INFO("value = " << value);
        CHECK(parse_test_pointer_drag(value).mode == Mode::disabled);
    }
}
