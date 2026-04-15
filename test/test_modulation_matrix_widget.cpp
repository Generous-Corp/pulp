// ModulationMatrixWidget interaction test — workstream 07 B5.
//
// Headless: simulates mouse clicks against a small widget and verifies
// the underlying ModulationMatrix receives the expected routes. No
// canvas backend needed — paint() is exercised elsewhere via
// screenshot tests.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/modulation_matrix.hpp>
#include <pulp/view/modulation_matrix_widget.hpp>

using namespace pulp::view;

namespace {
void configure_widget(ModulationMatrixWidget& w, ModulationMatrix& m) {
    w.set_bounds({0, 0, 400, 300});
    w.set_matrix(&m);
    w.set_sources({
        {"LFO 1", 1},
        {"Env 1", 2},
        {"Vel",   3},
    });
    w.set_destinations({
        {"Cutoff",    10},
        {"Resonance", 11},
        {"Volume",    12},
    });
}
} // namespace

TEST_CASE("Clicking a source sets the pending index", "[widget][modmatrix]") {
    ModulationMatrix m;
    ModulationMatrixWidget w;
    configure_widget(w, m);
    REQUIRE(w.pending_source() == -1);

    w.on_mouse_down({10, 50});  // row 0 of left column
    REQUIRE(w.pending_source() == 0);
}

TEST_CASE("Source → destination click pair adds a route", "[widget][modmatrix]") {
    ModulationMatrix m;
    ModulationMatrixWidget w;
    configure_widget(w, m);

    w.on_mouse_down({10, 50});   // source 0 (LFO 1)
    REQUIRE(w.pending_source() == 0);

    w.on_mouse_down({350, 150}); // destination 1 (Resonance)
    REQUIRE(w.pending_source() == -1);

    REQUIRE(m.size() == 1);
    REQUIRE(m.routes()[0].source == 1);
    REQUIRE(m.routes()[0].destination == 11);
    REQUIRE(m.routes()[0].depth == 1.0f);
}

TEST_CASE("Clicking destination without pending source is a no-op",
          "[widget][modmatrix]") {
    ModulationMatrix m;
    ModulationMatrixWidget w;
    configure_widget(w, m);
    w.on_mouse_down({350, 150}); // right column, no pending source
    REQUIRE(m.size() == 0);
    REQUIRE(w.pending_source() == -1);
}

TEST_CASE("A second pair appends to the matrix", "[widget][modmatrix]") {
    ModulationMatrix m;
    ModulationMatrixWidget w;
    configure_widget(w, m);

    w.on_mouse_down({10, 50});   // LFO 1
    w.on_mouse_down({350, 50});  // Cutoff

    w.on_mouse_down({10, 150});  // Env 1
    w.on_mouse_down({350, 250}); // Volume

    REQUIRE(m.size() == 2);
    REQUIRE(m.routes()[0].source == 1);
    REQUIRE(m.routes()[0].destination == 10);
    REQUIRE(m.routes()[1].source == 2);
    REQUIRE(m.routes()[1].destination == 12);
}
