// PreferencesPanel sidebar-of-pages container tests
//
// Validates the page registry + active-page selection contract:
//   - add_page wires content into the View tree,
//   - first registered page is active by default + visible,
//   - set_active_page(index) / (title) swap visibility and fire callback,
//   - persistence via PropertiesFile round-trips the last-selected page,
//   - bind_persistence honors stored value at bind time,
//   - bind_persistence with an unknown title leaves active page unchanged.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <pulp/state/properties_file.hpp>
#include <pulp/vellum_components/preferences_panel.hpp>
#include <vellum/view/view.hpp>

#include "render_helpers.hpp"

using namespace pulp::vellum_components;

namespace {
class TestView : public View {};

std::unique_ptr<TestView> make_view() {
    return std::make_unique<TestView>();
}
} // namespace

TEST_CASE("PreferencesPanel: defaults are empty + no active page",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    REQUIRE(p.page_count() == 0);
    REQUIRE(p.active_page() == -1);
    REQUIRE(p.active_page_title().empty());
    REQUIRE_FALSE(p.has_persistence());
}

TEST_CASE("PreferencesPanel: first registered page becomes active and visible",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    auto v1 = make_view();
    auto* raw1 = v1.get();
    p.add_page("General", "gear", std::move(v1));

    REQUIRE(p.page_count() == 1);
    REQUIRE(p.active_page() == 0);
    REQUIRE(p.active_page_title() == "General");
    REQUIRE(p.page_title(0) == "General");
    REQUIRE(p.page_icon(0) == "gear");
    REQUIRE(p.page_content(0) == raw1);
    REQUIRE(raw1->visible());
}

TEST_CASE("PreferencesPanel: subsequent pages register hidden",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    auto v1 = make_view();
    auto* raw1 = v1.get();
    auto v2 = make_view();
    auto* raw2 = v2.get();
    p.add_page("General", "gear", std::move(v1));
    p.add_page("Audio", "audio", std::move(v2));

    REQUIRE(p.page_count() == 2);
    REQUIRE(p.active_page() == 0);
    REQUIRE(raw1->visible());
    REQUIRE_FALSE(raw2->visible());
}

TEST_CASE("PreferencesPanel: set_active_page(index) swaps visibility",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    auto v1 = make_view();
    auto* raw1 = v1.get();
    auto v2 = make_view();
    auto* raw2 = v2.get();
    p.add_page("General", "gear", std::move(v1));
    p.add_page("Audio", "audio", std::move(v2));

    p.set_active_page(1);
    REQUIRE(p.active_page() == 1);
    REQUIRE(p.active_page_title() == "Audio");
    REQUIRE_FALSE(raw1->visible());
    REQUIRE(raw2->visible());

    p.set_active_page(0);
    REQUIRE(raw1->visible());
    REQUIRE_FALSE(raw2->visible());
}

TEST_CASE("PreferencesPanel: set_active_page(title) selects by name",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    p.add_page("General", "gear", make_view());
    p.add_page("Audio", "audio", make_view());
    p.add_page("MIDI", "midi", make_view());

    REQUIRE(p.set_active_page("MIDI"));
    REQUIRE(p.active_page() == 2);
    REQUIRE(p.active_page_title() == "MIDI");

    REQUIRE_FALSE(p.set_active_page("Bogus"));
    REQUIRE(p.active_page() == 2); // unchanged
}

TEST_CASE("PreferencesPanel: out-of-range set_active_page is a no-op",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    p.add_page("General", "gear", make_view());
    p.set_active_page(5);
    REQUIRE(p.active_page() == 0);
    p.set_active_page(-1);
    REQUIRE(p.active_page() == 0);
}

TEST_CASE("PreferencesPanel: on_page_change fires for transitions",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    std::vector<int> seen;
    p.on_page_change = [&](int idx) { seen.push_back(idx); };
    p.add_page("General", "gear", make_view()); // first → fires 0
    p.add_page("Audio", "audio", make_view());
    p.set_active_page(1); // fires 1
    p.set_active_page(1); // duplicate, no fire
    p.set_active_page(0); // fires 0

    REQUIRE(seen.size() == 3);
    REQUIRE(seen[0] == 0);
    REQUIRE(seen[1] == 1);
    REQUIRE(seen[2] == 0);
}

TEST_CASE("PreferencesPanel: persistence writes selected page to PropertiesFile",
          "[pulp-vellum-components][preferences-panel]") {
    pulp::state::PropertiesFile props;
    PreferencesPanel p;
    p.add_page("General", "gear", make_view());
    p.add_page("Audio", "audio", make_view());

    p.bind_persistence(&props);
    REQUIRE(p.has_persistence());
    REQUIRE(p.persistence_key() == "preferences.active_page");

    p.set_active_page(1);
    auto stored = props.get_string("preferences.active_page");
    REQUIRE(stored.has_value());
    REQUIRE(*stored == "Audio");

    p.set_active_page(0);
    REQUIRE(props.get_string("preferences.active_page") == std::optional<std::string>("General"));
}

TEST_CASE("PreferencesPanel: bind_persistence selects stored page at bind time",
          "[pulp-vellum-components][preferences-panel]") {
    pulp::state::PropertiesFile props;
    props.set_string("preferences.active_page", "Audio");

    PreferencesPanel p;
    p.add_page("General", "gear", make_view());
    p.add_page("Audio", "audio", make_view());

    p.bind_persistence(&props);
    REQUIRE(p.active_page() == 1);
    REQUIRE(p.active_page_title() == "Audio");
}

TEST_CASE("PreferencesPanel: bind_persistence leaves active unchanged for missing title",
          "[pulp-vellum-components][preferences-panel]") {
    pulp::state::PropertiesFile props;
    props.set_string("preferences.active_page", "DeletedPage");

    PreferencesPanel p;
    p.add_page("General", "gear", make_view());
    p.add_page("Audio", "audio", make_view());

    p.bind_persistence(&props);
    // No matching page → stays on default (first registered).
    REQUIRE(p.active_page() == 0);
}

TEST_CASE("PreferencesPanel: custom persistence key is honored",
          "[pulp-vellum-components][preferences-panel]") {
    pulp::state::PropertiesFile props;
    PreferencesPanel p;
    p.set_persistence_key("mysettings.tab");
    p.add_page("General", "gear", make_view());
    p.add_page("Audio", "audio", make_view());
    p.bind_persistence(&props);

    p.set_active_page(1);
    REQUIRE(props.get_string("mysettings.tab") == std::optional<std::string>("Audio"));
    REQUIRE_FALSE(props.get_string("preferences.active_page").has_value());
}

TEST_CASE("PreferencesPanel: simulate_select drives selection like a sidebar click",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    p.add_page("General", "gear", make_view());
    p.add_page("Audio", "audio", make_view());
    p.simulate_select(1);
    REQUIRE(p.active_page() == 1);
}

TEST_CASE("PreferencesPanel: only the active page's content reaches the pixels",
          "[pulp-vellum-components][preferences-panel]") {
    PreferencesPanel p;
    p.set_bounds({0, 0, 100, 60});
    p.flex().direction = ::vellum::view::FlexDirection::column;
    auto general = std::make_unique<View>();
    general->set_background_color(::vellum::canvas::Color::rgba8(200, 40, 40, 255));
    general->flex().flex_grow = 1.0f;
    auto audio = std::make_unique<View>();
    audio->set_background_color(::vellum::canvas::Color::rgba8(40, 200, 40, 255));
    audio->flex().flex_grow = 1.0f;
    p.add_page("General", "gear", std::move(general));
    p.add_page("Audio", "audio", std::move(audio));

    CHECK(testing::near(testing::render(p, 100, 60).at(50, 30), 200, 40, 40));
    testing::maybe_write_png(p, 100, 60, "preferences-general.png");
    p.set_active_page("Audio");
    CHECK(testing::near(testing::render(p, 100, 60).at(50, 30), 40, 200, 40));
    testing::maybe_write_png(p, 100, 60, "preferences-audio.png");
}
