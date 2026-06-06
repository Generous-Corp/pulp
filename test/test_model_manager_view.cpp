// ModelManagerView (MM-PR3) — builds a block per model (header + subtitle + N blocks),
// shows a progress bar line while downloading, fires the action callbacks, and renders
// non-blank on the Skia canvas.
#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/model_store.hpp>
#include <pulp/view/model_manager_view.hpp>
#include <pulp/view/screenshot.hpp>

#include <fstream>
#include <string>

using namespace pulp::view;
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

    r.models = {a, b};  // index 0 active+installed, index 1 available
    return r;
}

// Children are: [0] header, [1] subtitle, [2..] one block per model.
View* model_block(View& v, size_t i) { return v.child_at(2 + i); }

void fire_buttons(View* view) {
    if (auto* b = dynamic_cast<ToggleButton*>(view))
        if (b->on_toggle) b->on_toggle(true);
    for (size_t i = 0; i < view->child_count(); ++i) fire_buttons(view->child_at(i));
}

}  // namespace

TEST_CASE("ModelManagerView builds a block per model", "[view][model]") {
    ModelManagerView v;
    v.set_models(make_models());
    REQUIRE(v.child_count() == 4);  // header + subtitle + 2 model blocks
}

TEST_CASE("ModelManagerView adds a progress-bar line while downloading", "[view][model]") {
    ModelManagerView v;
    v.set_models(make_models());

    const size_t before = model_block(v, 1)->child_count();  // mrt2_small: just the line
    v.set_download_progress("mrt2_small", 0.47f);
    REQUIRE(model_block(v, 1)->child_count() > before);  // gained the full-width bar line
}

TEST_CASE("ModelManagerView action callbacks fire with the model id", "[view][model]") {
    ModelManagerView v;
    std::string downloaded;
    std::string removed;
    v.on_download = [&](const std::string& id) { downloaded = id; };
    v.on_remove = [&](const std::string& id) { removed = id; };
    v.set_models(make_models());

    fire_buttons(model_block(v, 0));  // mrt2_base installed+active → Remove
    fire_buttons(model_block(v, 1));  // mrt2_small not installed → Download

    REQUIRE(downloaded == "mrt2_small");
    REQUIRE(removed == "mrt2_base");
}

TEST_CASE("ModelManagerView shows a header Done when closeable", "[view][model]") {
    ModelManagerView v;
    bool done = false;
    v.on_done = [&] { done = true; };
    v.set_models(make_models());
    v.set_can_close(true);
    fire_buttons(v.child_at(0));  // header contains the Done button
    REQUIRE(done);
}

TEST_CASE("ModelManagerView renders non-blank on the Skia canvas", "[view][model][.demo]") {
    ModelManagerView v;
    v.on_done = [] {};
    v.set_models(make_models());
    v.set_can_close(true);
    v.set_download_progress("mrt2_small", 0.47f);

    auto png = render_to_png(v, 620, 300, 2.0f, ScreenshotBackend::skia);
    REQUIRE_FALSE(png.empty());
    std::ofstream("/tmp/model-manager.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}
