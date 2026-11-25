// ModelManagerView on the Vellum view layer: a block per model, a progress bar line
// while downloading, action callbacks carrying the model id, and a real raster.
#include <catch2/catch_test_macros.hpp>

#include "render_helpers.hpp"
#include <pulp/runtime/model_store.hpp>
#include <pulp/vellum_components/model_manager_view.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pulp::vellum_components;
using pulp::runtime::ListedModel;
using pulp::runtime::ModelListResult;

namespace {

ModelListResult make_models() {
    ModelListResult r;
    r.active_model_id = "mrt2_base";

    ListedModel a;
    a.model.model_id = "mrt2_base";
    a.model.display_name = "Magenta RT2 (Large)";
    a.model.is_recommended = true;
    a.status = "installed";
    a.active = true;

    ListedModel b;
    b.model.model_id = "mrt2_small";
    b.model.display_name = "Magenta RT2 (Small)";
    b.status = "not_installed";

    r.models = {a, b}; // index 0 active+installed, index 1 available
    return r;
}

// Children are: [0] header, [1] subtitle, [2..] one block per model.
View* model_block(View& v, size_t i) {
    return v.child_at(2 + i);
}

void fire_buttons(View* view) {
    if (auto* b = dynamic_cast<ToggleButton*>(view))
        if (b->on_toggle)
            b->on_toggle(true);
    if (auto* t = dynamic_cast<TextButton*>(view)) // action buttons are momentary TextButtons
        if (t->on_click)
            t->on_click();
    for (size_t i = 0; i < view->child_count(); ++i)
        fire_buttons(view->child_at(i));
}

} // namespace

TEST_CASE("ModelManagerView builds a block per model", "[pulp-vellum-components][model-manager]") {
    ModelManagerView v;
    v.set_models(make_models());
    REQUIRE(v.child_count() == 4); // header + subtitle + 2 model blocks
}

TEST_CASE("ModelManagerView adds a progress-bar line while downloading",
          "[pulp-vellum-components][model-manager]") {
    ModelManagerView v;
    v.set_models(make_models());

    const size_t before = model_block(v, 1)->child_count(); // mrt2_small: just the line
    v.set_download_progress("mrt2_small", 0.47f);
    REQUIRE(model_block(v, 1)->child_count() > before); // gained the full-width bar line
}

TEST_CASE("ModelManagerView action callbacks fire with the model id",
          "[pulp-vellum-components][model-manager]") {
    ModelManagerView v;
    std::string downloaded;
    std::string removed;
    v.on_download = [&](const std::string& id) { downloaded = id; };
    v.on_remove = [&](const std::string& id) { removed = id; };
    v.set_models(make_models());

    fire_buttons(model_block(v, 0)); // mrt2_base installed+active → Remove
    fire_buttons(model_block(v, 1)); // mrt2_small not installed → Download

    REQUIRE(downloaded == "mrt2_small");
    REQUIRE(removed == "mrt2_base");
}

TEST_CASE("ModelManagerView shows a header Done when closeable",
          "[pulp-vellum-components][model-manager]") {
    ModelManagerView v;
    bool done = false;
    v.on_done = [&] { done = true; };
    v.set_models(make_models());
    v.set_can_close(true);
    fire_buttons(v.child_at(0)); // header contains the Done button
    REQUIRE(done);
}

using namespace pulp::vellum_components::testing;

namespace {

bool has_color(const Frame& frame, int r, int g, int b, int tol) {
    for (std::uint32_t y = 0; y < frame.height; ++y)
        for (std::uint32_t x = 0; x < frame.width; ++x)
            if (near(frame.at(x, y), r, g, b, tol))
                return true;
    return false;
}

} // namespace

TEST_CASE("ModelManagerView renders its panel, rows and progress fill",
          "[pulp-vellum-components][model-manager]") {
    ModelManagerView idle;
    idle.on_done = [] {};
    idle.set_models(make_models());
    idle.set_can_close(true);
    idle.set_bounds({0, 0, 620, 300});
    const auto before = render(idle, 620, 300);
    REQUIRE(before.width == 620);
    // The padding shows the panel background (32, 33, 36); with no download the
    // progress accent (127, 178, 255) appears nowhere.
    CHECK(near(before.at(4, 4), 32, 33, 36, 2));
    CHECK_FALSE(has_color(before, 127, 178, 255, 3));

    ModelManagerView busy;
    busy.on_done = [] {};
    busy.set_models(make_models());
    busy.set_can_close(true);
    busy.set_download_progress("mrt2_small", 0.47f);
    busy.set_bounds({0, 0, 620, 300});
    const auto during = render(busy, 620, 300);
    CHECK(has_color(during, 127, 178, 255, 3)); // fill
    CHECK(has_color(during, 55, 56, 60, 2));    // track
    maybe_write_png(busy, 620, 300, "model-manager.png");
}
