// PropertyPanel typed-variant tests
// (closes the gap-doc Phase 4 row "PreferencesPanel + PropertyPanel"
//  for the PropertyPanel half).
//
// Validates:
//   - the six typed variants (Boolean, Choice, Slider, Button,
//     MultiChoice, Text) register + round-trip values,
//   - simulate_* helpers fire the same path a real UI event would,
//   - bounds + clamping for slider + choice + multi-choice,
//   - PropertiesFile persistence round-trips every variant under each
//     property's key,
//   - bind_persistence with an existing store seeds the panel from
//     stored values,
//   - re-registering a key replaces the old entry in place.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <pulp/state/properties_file.hpp>
#include <pulp/view/property_panel.hpp>

using namespace pulp::view;

namespace {

std::filesystem::path scratch_props_file(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() / "pulp-property-panel-tests";
    std::filesystem::create_directories(dir);
    auto path = dir / (tag + ".json");
    std::filesystem::remove(path);
    return path;
}

} // namespace

// ── Variant builders + reads ─────────────────────────────────────────

TEST_CASE("PropertyPanel: defaults are empty", "[property-panel]") {
    PropertyPanel p;
    REQUIRE(p.empty());
    REQUIRE(p.size() == 0);
    REQUIRE_FALSE(p.has_persistence());
    REQUIRE(p.find("missing") == nullptr);
}

TEST_CASE("PropertyPanel: add_boolean stores value + fires observer",
          "[property-panel][boolean]") {
    PropertyPanel p;
    bool last = false;
    int calls = 0;
    p.add_boolean("audio.mono", "Force Mono", true,
                  [&](bool v) { last = v; ++calls; });

    REQUIRE(p.size() == 1);
    REQUIRE(p.get_bool("audio.mono").value_or(false));

    REQUIRE(p.set_bool("audio.mono", false));
    REQUIRE_FALSE(p.get_bool("audio.mono").value_or(true));
    REQUIRE(calls == 1);
    REQUIRE_FALSE(last);

    REQUIRE(p.simulate_toggle("audio.mono"));
    REQUIRE(p.get_bool("audio.mono").value_or(false));
    REQUIRE(calls == 2);
    REQUIRE(last);
}

TEST_CASE("PropertyPanel: add_choice clamps index + fires observer",
          "[property-panel][choice]") {
    PropertyPanel p;
    int last = -1;
    p.add_choice("audio.driver", "Driver",
                 {"CoreAudio", "JACK", "PortAudio"}, 10,
                 [&](int i) { last = i; });

    // Initial clamped down to 2 (3 options, index 0..2).
    REQUIRE(p.get_choice("audio.driver") == 2);

    REQUIRE(p.simulate_choose("audio.driver", 1));
    REQUIRE(p.get_choice("audio.driver") == 1);
    REQUIRE(last == 1);

    REQUIRE(p.simulate_choose("audio.driver", -5));
    REQUIRE(p.get_choice("audio.driver") == 0);
    REQUIRE(last == 0);
}

TEST_CASE("PropertyPanel: add_slider clamps to min/max",
          "[property-panel][slider]") {
    PropertyPanel p;
    float last = 0.0f;
    p.add_slider("audio.gain", "Input gain", -60.0f, 12.0f, 100.0f,
                 [&](float v) { last = v; });

    REQUIRE(p.get_slider("audio.gain") == 12.0f);  // clamped initial

    REQUIRE(p.simulate_slide("audio.gain", -200.0f));
    REQUIRE(p.get_slider("audio.gain") == -60.0f);
    REQUIRE(last == -60.0f);

    REQUIRE(p.simulate_slide("audio.gain", 0.0f));
    REQUIRE(p.get_slider("audio.gain") == 0.0f);
}

TEST_CASE("PropertyPanel: add_button fires click handler",
          "[property-panel][button]") {
    PropertyPanel p;
    int clicks = 0;
    p.add_button("audio.reset", "Reset", [&] { ++clicks; });

    REQUIRE(p.size() == 1);
    REQUIRE(p.simulate_click("audio.reset"));
    REQUIRE(p.simulate_click("audio.reset"));
    REQUIRE(clicks == 2);

    // Wrong kind / missing key — no-op + false.
    REQUIRE_FALSE(p.simulate_click("does.not.exist"));
}

TEST_CASE("PropertyPanel: multi_choice normalizes selection + observer",
          "[property-panel][multi-choice]") {
    PropertyPanel p;
    std::vector<int> last;
    p.add_multi_choice("ports.in", "Input ports",
                       {"L", "R", "FX1", "FX2"}, {3, 0, 0, 5},
                       [&](const std::vector<int>& xs) { last = xs; });

    // Dedup + clamp: 5 is out of range, double 0 collapses → {0, 3}.
    auto initial = p.get_multi_choice("ports.in").value();
    REQUIRE(initial == std::vector<int>{0, 3});

    REQUIRE(p.simulate_multi_check("ports.in", 2, true));
    REQUIRE(p.get_multi_choice("ports.in") == std::vector<int>{0, 2, 3});
    REQUIRE(last == std::vector<int>{0, 2, 3});

    REQUIRE(p.simulate_multi_check("ports.in", 0, false));
    REQUIRE(p.get_multi_choice("ports.in") == std::vector<int>{2, 3});

    // Out-of-range simulate_multi_check still normalizes.
    REQUIRE(p.simulate_multi_check("ports.in", 99, true));
    REQUIRE(p.get_multi_choice("ports.in") == std::vector<int>{2, 3});
}

TEST_CASE("PropertyPanel: text variant", "[property-panel][text]") {
    PropertyPanel p;
    std::string last;
    p.add_text("user.name", "Name", "Anonymous",
               [&](const std::string& v) { last = v; });

    REQUIRE(p.get_text("user.name") == "Anonymous");
    REQUIRE(p.simulate_set_text("user.name", "Alice"));
    REQUIRE(p.get_text("user.name") == "Alice");
    REQUIRE(last == "Alice");
}

TEST_CASE("PropertyPanel: typed setters reject mismatched kinds",
          "[property-panel][type-safety]") {
    PropertyPanel p;
    p.add_text("a", "A", "x");
    REQUIRE_FALSE(p.set_bool("a", true));
    REQUIRE_FALSE(p.set_choice("a", 0));
    REQUIRE_FALSE(p.set_slider("a", 0.5f));
    REQUIRE_FALSE(p.set_multi_choice("a", {0}));
    REQUIRE_FALSE(p.click("a"));
    REQUIRE(p.get_text("a") == "x");  // value untouched
}

TEST_CASE("PropertyPanel: re-registering a key replaces the slot in place",
          "[property-panel][registry]") {
    PropertyPanel p;
    p.add_text("name", "Name", "first");
    p.add_text("name", "Name", "second");
    REQUIRE(p.size() == 1);
    REQUIRE(p.get_text("name") == "second");
}

// ── Persistence ───────────────────────────────────────────────────────

TEST_CASE("PropertyPanel: persistence round-trips all typed variants",
          "[property-panel][persistence]") {
    auto path = scratch_props_file("roundtrip");
    pulp::state::PropertiesFile out;

    {
        PropertyPanel p;
        p.add_boolean("b", "B", false);
        p.add_choice("c", "C", {"a", "b", "c"}, 0);
        p.add_slider("s", "S", 0.0f, 1.0f, 0.25f);
        p.add_multi_choice("mc", "MC", {"x", "y", "z"}, {});
        p.add_text("t", "T", "");

        p.bind_persistence(&out);
        REQUIRE(p.has_persistence());

        p.set_bool("b", true);
        p.set_choice("c", 2);
        p.set_slider("s", 0.75f);
        p.set_multi_choice("mc", {0, 2});
        p.set_text("t", "value");
    }

    REQUIRE(out.get_bool("b") == true);
    REQUIRE(out.get_int("c") == 2);
    REQUIRE(out.get_double("s") == 0.75);
    REQUIRE(out.get_string("mc") == std::string("0,2"));
    REQUIRE(out.get_string("t") == std::string("value"));

    // Save → reload through a fresh panel, verify state restores.
    REQUIRE(out.save(path.string()));
    pulp::state::PropertiesFile reloaded;
    REQUIRE(reloaded.load(path.string()));

    PropertyPanel q;
    q.add_boolean("b", "B", false);
    q.add_choice("c", "C", {"a", "b", "c"}, 0);
    q.add_slider("s", "S", 0.0f, 1.0f, 0.0f);
    q.add_multi_choice("mc", "MC", {"x", "y", "z"}, {});
    q.add_text("t", "T", "");
    q.bind_persistence(&reloaded);

    REQUIRE(q.get_bool("b") == true);
    REQUIRE(q.get_choice("c") == 2);
    REQUIRE(q.get_slider("s") == 0.75f);
    REQUIRE(q.get_multi_choice("mc") == std::vector<int>{0, 2});
    REQUIRE(q.get_text("t") == "value");

    std::filesystem::remove(path);
}

TEST_CASE("PropertyPanel: bind_persistence clamps stored out-of-range Choice",
          "[property-panel][persistence][defensive]") {
    pulp::state::PropertiesFile pf;
    pf.set_int("c", 99);  // someone hand-edited the JSON

    PropertyPanel p;
    p.add_choice("c", "C", {"a", "b"}, 0);
    p.bind_persistence(&pf);
    REQUIRE(p.get_choice("c") == 1);  // clamped
}

TEST_CASE("PropertyPanel: bind_persistence(nullptr) detaches without erasing",
          "[property-panel][persistence]") {
    pulp::state::PropertiesFile pf;
    PropertyPanel p;
    p.add_text("t", "T", "before");
    p.bind_persistence(&pf);
    p.set_text("t", "after");
    REQUIRE(pf.get_string("t") == std::string("after"));

    p.bind_persistence(nullptr);
    REQUIRE_FALSE(p.has_persistence());

    p.set_text("t", "later");
    // Detached — store still has the old value.
    REQUIRE(pf.get_string("t") == std::string("after"));
    REQUIRE(p.get_text("t") == "later");
}

TEST_CASE("PropertyPanel: decode_indices tolerates malformed entries",
          "[property-panel][persistence][defensive]") {
    pulp::state::PropertiesFile pf;
    pf.set_string("mc", "0,not-a-num, 2 ,99");

    PropertyPanel p;
    p.add_multi_choice("mc", "MC", {"a", "b", "c"}, {});
    p.bind_persistence(&pf);

    auto xs = p.get_multi_choice("mc").value();
    // 0 and 2 are valid; "not-a-num" skipped; 99 dropped; result sorted.
    REQUIRE(xs == std::vector<int>{0, 2});
}
