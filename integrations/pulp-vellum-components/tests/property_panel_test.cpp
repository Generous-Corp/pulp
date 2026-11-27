// PropertyPanel typed-variant tests.
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#include <pulp/state/properties_file.hpp>
#include <pulp/vellum_components/property_panel.hpp>

using namespace pulp::vellum_components;

namespace {

// Unique per process and per call: two runs of this binary at once (parallel
// CTest entries, or two checkouts on one host) must not save over each
// other's file between a save and its reload.
std::filesystem::path scratch_props_file(const std::string& tag) {
    static std::atomic<unsigned long long> counter{0};
#if defined(_WIN32)
    const auto pid = static_cast<std::uint64_t>(_getpid());
#else
    const auto pid = static_cast<std::uint64_t>(::getpid());
#endif
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / "pulp-property-panel-tests";
    std::filesystem::create_directories(dir);
    auto path = dir / (tag + "-" + std::to_string(pid) + "-" + std::to_string(tick) + "-" +
                       std::to_string(counter.fetch_add(1)) + ".json");
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

TEST_CASE("PropertyPanel: add_boolean stores value + fires observer", "[property-panel][boolean]") {
    PropertyPanel p;
    bool last = false;
    int calls = 0;
    p.add_boolean("audio.mono", "Force Mono", true, [&](bool v) {
        last = v;
        ++calls;
    });

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

TEST_CASE("PropertyPanel: add_choice clamps index + fires observer", "[property-panel][choice]") {
    PropertyPanel p;
    int last = -1;
    p.add_choice("audio.driver", "Driver", {"CoreAudio", "JACK", "PortAudio"}, 10,
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

TEST_CASE("PropertyPanel: add_slider clamps to min/max", "[property-panel][slider]") {
    PropertyPanel p;
    float last = 0.0f;
    p.add_slider("audio.gain", "Input gain", -60.0f, 12.0f, 100.0f, [&](float v) { last = v; });

    REQUIRE(p.get_slider("audio.gain") == 12.0f); // clamped initial

    REQUIRE(p.simulate_slide("audio.gain", -200.0f));
    REQUIRE(p.get_slider("audio.gain") == -60.0f);
    REQUIRE(last == -60.0f);

    REQUIRE(p.simulate_slide("audio.gain", 0.0f));
    REQUIRE(p.get_slider("audio.gain") == 0.0f);
}

TEST_CASE("PropertyPanel: add_button fires click handler", "[property-panel][button]") {
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
    p.add_multi_choice("ports.in", "Input ports", {"L", "R", "FX1", "FX2"}, {3, 0, 0, 5},
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
    p.add_text("user.name", "Name", "Anonymous", [&](const std::string& v) { last = v; });

    REQUIRE(p.get_text("user.name") == "Anonymous");
    REQUIRE(p.simulate_set_text("user.name", "Alice"));
    REQUIRE(p.get_text("user.name") == "Alice");
    REQUIRE(last == "Alice");
}

TEST_CASE("PropertyPanel: typed setters reject mismatched kinds", "[property-panel][type-safety]") {
    PropertyPanel p;
    p.add_text("a", "A", "x");
    REQUIRE_FALSE(p.set_bool("a", true));
    REQUIRE_FALSE(p.set_choice("a", 0));
    REQUIRE_FALSE(p.set_slider("a", 0.5f));
    REQUIRE_FALSE(p.set_multi_choice("a", {0}));
    REQUIRE_FALSE(p.click("a"));
    REQUIRE(p.get_text("a") == "x"); // value untouched
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
    pf.set_int("c", 99); // someone hand-edited the JSON

    PropertyPanel p;
    p.add_choice("c", "C", {"a", "b"}, 0);
    p.bind_persistence(&pf);
    REQUIRE(p.get_choice("c") == 1); // clamped
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

TEST_CASE("PropertyPanel: multi_choice with no options keeps no indices",
          "[property-panel][multi-choice][defensive]") {
    for (int idx : {0, -1, 5}) {
        INFO("index " << idx);
        PropertyPanel p;
        p.add_multi_choice("mc", "MC", {}, {idx});
        REQUIRE(p.get_multi_choice("mc").value().empty());

        REQUIRE(p.set_multi_choice("mc", {idx}));
        REQUIRE(p.get_multi_choice("mc").value().empty());

        pulp::state::PropertiesFile pf;
        pf.set_string("mc", std::to_string(idx));
        PropertyPanel q;
        q.add_multi_choice("mc", "MC", {}, {});
        q.bind_persistence(&pf);
        REQUIRE(q.get_multi_choice("mc").value().empty());
    }

    // Control: a non-empty option list still keeps its valid indices.
    PropertyPanel c;
    c.add_multi_choice("mc", "MC", {"a", "b"}, {1, 0});
    REQUIRE(c.get_multi_choice("mc") == std::vector<int>{0, 1});
    REQUIRE(c.set_multi_choice("mc", {1, 7, -1}));
    REQUIRE(c.get_multi_choice("mc") == std::vector<int>{1});
}

TEST_CASE("PropertyPanel: every member the header documents exists", "[property-panel][docs]") {
    const std::filesystem::path header = std::filesystem::path(PULP_VELLUM_COMPONENTS_INCLUDE_DIR) /
                                         "pulp" / "vellum_components" / "property_panel.hpp";
    std::ifstream in(header);
    REQUIRE(in.good());

    std::string doc, code;
    for (std::string line; std::getline(in, line);) {
        const auto first = line.find_first_not_of(" \t");
        const bool is_doc = first != std::string::npos && line.compare(first, 3, "///") == 0;
        (is_doc ? doc : code) += line + "\n";
    }

    // Every `name(` or `name` a doc comment backticks as a member must be
    // declared in the code, so a consumer following the contract compiles.
    std::set<std::string> documented;
    const std::regex call(R"re(`((?:get|set|add|simulate|bind)_[a-z_]*|value_for)(?:\(|`))re");
    for (std::sregex_iterator it(doc.begin(), doc.end(), call), end; it != end; ++it)
        documented.insert((*it)[1]);
    REQUIRE_FALSE(documented.empty());
    for (const auto& name : documented)
        CHECK(code.find(name + "(") != std::string::npos);
}

TEST_CASE("PropertyPanel: observer that registers properties does not dangle",
          "[property-panel][lifetime]") {
    pulp::state::PropertiesFile props;
    PropertyPanel panel;
    panel.bind_persistence(&props);
    int seen = -1;
    panel.add_text("a", "A", "x", [&](const std::string& v) {
        // Force properties_ to reallocate while fire() is mid-flight, and
        // replace the firing slot itself (destroys the running callback).
        for (int i = 0; i < 64; ++i)
            panel.add_boolean("dep" + std::to_string(i), "D", false);
        panel.add_text("a", "A", v);
        seen = static_cast<int>(v.size());
    });
    REQUIRE(panel.set_text("a", "hello"));
    CHECK(seen == 5);
    CHECK(panel.size() == 65);
    CHECK(props.get_string("a").value_or("") == "hello");
    panel.add_button("b", "B", [&] {
        for (int i = 0; i < 64; ++i)
            panel.add_boolean("more" + std::to_string(i), "M", false);
        panel.add_button("b", "B", [] {});
    });
    CHECK(panel.click("b"));
}

TEST_CASE("PropertyPanel: observer re-registering its own key keeps the pending change",
          "[property-panel][lifetime]") {
    pulp::state::PropertiesFile props;
    props.set_string("a", "old");
    PropertyPanel panel;
    panel.bind_persistence(&props);
    panel.add_text("a", "A", "x", [&](const std::string& v) { panel.add_text("a", "A", v); });
    REQUIRE(panel.set_text("a", "new"));
    CHECK(panel.get_text("a").value_or("") == "new");
    CHECK(props.get_string("a").value_or("") == "new");
}

TEST_CASE("PropertyPanel: stored Choice index beyond int range clamps, not wraps",
          "[property-panel][persistence]") {
    pulp::state::PropertiesFile props;
    props.set_int("c", int64_t{4294967296});
    props.set_int("n", int64_t{-4294967296});
    PropertyPanel panel;
    panel.bind_persistence(&props);
    panel.add_choice("c", "C", {"a", "b", "c"}, 0);
    panel.add_choice("n", "N", {"a", "b", "c"}, 2);
    CHECK(panel.get_choice("c").value_or(-1) == 2);
    CHECK(panel.get_choice("n").value_or(-1) == 0);
}

TEST_CASE("PropertyPanel: mutable callback state survives across events and nested rebuild keeps pending key",
          "[property-panel][lifetime]") {
    pulp::state::PropertiesFile props;
    props.set_string("a", "old");
    PropertyPanel panel;
    panel.bind_persistence(&props);
    int clicks = 0;
    panel.add_button("b", "B", [n = 0, &clicks]() mutable { clicks = ++n; });
    panel.click("b");
    panel.click("b");
    CHECK(clicks == 2);
    panel.add_text("a", "A", "x", [&](const std::string&) { panel.set_text("b2", "go"); });
    panel.add_text("b2", "B2", "", [&](const std::string&) {
        panel.add_text("a", "A", "new");
    });
    REQUIRE(panel.set_text("a", "new"));
    CHECK(panel.get_text("a").value_or("") == "new");
    CHECK(props.get_string("a").value_or("") == "new");
}

TEST_CASE("PropertyPanel: retired callbacks are not resurrected; NaN slider rejected",
          "[property-panel][lifetime]") {
    PropertyPanel panel;
    int old_hits = 0;
    int click_hits = 0;
    panel.add_text("t", "T", "", [&](const std::string&) {
        panel.add_text("t", "T", "replaced"); // replacement has no observer
        ++old_hits;
    });
    panel.set_text("t", "1");
    panel.set_text("t", "2");
    CHECK(old_hits == 1);
    panel.add_button("b", "B", [&] {
        panel.add_button("b", "B", {});
        ++click_hits;
    });
    panel.click("b");
    panel.click("b");
    CHECK(click_hits == 1);
    panel.add_slider("s", "S", 0.0f, 1.0f, 0.5f);
    panel.set_slider("s", std::numeric_limits<float>::quiet_NaN());
    CHECK(panel.get_slider("s").value_or(-1.0f) == 0.0f);
}
