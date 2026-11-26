// The catalogue builds each retained component by stable id and hands back a
// Vellum view.

#include <pulp/vellum_components/catalogue.hpp>
#include <pulp/vellum_components/lasso.hpp>
#include <pulp/vellum_components/model_manager_view.hpp>
#include <pulp/vellum_components/preferences_panel.hpp>
#include <pulp/vellum_components/preset_browser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace pulp::vellum_components;

namespace {

PulpViewComponentContext serviced_context(int* requests) {
    PulpViewComponentContext ctx;
    ctx.panic.request = [requests](bool) { ++*requests; };
    ctx.panic.availability = [] { return PanicAvailability::voices_only; };
    return ctx;
}

} // namespace

TEST_CASE("every catalogue id constructs a Vellum view of its component type",
          "[pulp-vellum-components][catalogue]") {
    int requests = 0;
    const auto ctx = serviced_context(&requests);

    const std::vector<std::string_view> expected = {"pulp.panic_button", "pulp.lasso",
                                                    "pulp.preferences_panel", "pulp.model_manager",
                                                    "pulp.preset_browser"};
    REQUIRE(pulp_view_component_ids() == expected);

    auto panic = make_pulp_view_component("pulp.panic_button", ctx);
    auto lasso = make_pulp_view_component("pulp.lasso", ctx);
    auto prefs = make_pulp_view_component("pulp.preferences_panel", ctx);
    REQUIRE(panic);
    REQUIRE(lasso);
    auto models = make_pulp_view_component("pulp.model_manager", ctx);
    REQUIRE(prefs);
    REQUIRE(models);
    CHECK(dynamic_cast<ModelManagerView*>(models.get()));
    CHECK(dynamic_cast<PanicButton*>(panic.get()));
    CHECK(dynamic_cast<LassoComponent*>(lasso.get()));
    CHECK(dynamic_cast<PreferencesPanel*>(prefs.get()));
    static_assert(std::is_base_of_v<::vellum::view::View, PanicButton>);
}

TEST_CASE("an unknown id yields no component", "[pulp-vellum-components][catalogue]") {
    const PulpViewComponentContext ctx;
    CHECK(make_pulp_view_component("pulp.nonexistent", ctx) == nullptr);
    CHECK(make_pulp_view_component("", ctx) == nullptr);
}

TEST_CASE("the context reaches the panic button", "[pulp-vellum-components][catalogue]") {
    int requests = 0;
    auto view = make_pulp_view_component("pulp.panic_button", serviced_context(&requests));
    auto* button = dynamic_cast<PanicButton*>(view.get());
    REQUIRE(button);
    CHECK(button->trigger());
    CHECK(requests == 1);

    auto bare = make_pulp_view_component("pulp.panic_button", {});
    auto* inert = dynamic_cast<PanicButton*>(bare.get());
    REQUIRE(inert);
    CHECK_FALSE(inert->trigger());
}

TEST_CASE("a catalogue preferences panel restores the stored page once bound",
          "[pulp-vellum-components][catalogue]") {
    pulp::state::PropertiesFile props;
    props.set_string("preferences.active_page", "Audio");

    auto view = make_pulp_view_component("pulp.preferences_panel", {});
    auto* panel = dynamic_cast<PreferencesPanel*>(view.get());
    REQUIRE(panel);
    panel->add_page("General", "gear", std::make_unique<::vellum::view::View>());
    panel->add_page("Audio", "audio", std::make_unique<::vellum::view::View>());
    CHECK(panel->active_page_title() == "General");

    // Binding after the pages exist selects the stored one.
    panel->bind_persistence(&props);
    CHECK(panel->active_page_title() == "Audio");

    // Later selections write back.
    CHECK(panel->set_active_page(std::string_view("General")));
    CHECK(props.get_string("preferences.active_page") == std::optional<std::string>("General"));
}

TEST_CASE("the catalogue builds a preset browser over the context's manager",
          "[pulp-vellum-components][catalogue]") {
    pulp::state::StateStore store;
    pulp::state::PresetManager manager(store, "CatalogueCo", "CatalogueBrowser");

    PulpViewComponentContext ctx;
    CHECK(make_pulp_view_component("pulp.preset_browser", ctx) == nullptr);

    ctx.presets = &manager;
    auto view = make_pulp_view_component("pulp.preset_browser", ctx);
    REQUIRE(view);
    CHECK(dynamic_cast<PresetBrowser*>(view.get()));
}
