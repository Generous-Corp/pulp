// Plugin-contributed settings sections (MM-PR5). Verifies the contract that lets a
// plugin surface its own Settings tabs (e.g. a model picker) which the host composes
// alongside its own host-owned Audio/MIDI tabs — keeping device selection a host concern
// while giving one unified Settings panel.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>
#include <string>
#include <vector>

using pulp::format::Processor;
using pulp::format::SettingsPanel;

namespace {

// Minimal concrete Processor whose contributed sections are configurable per test.
struct TestProcessor : Processor {
    std::vector<std::string> section_titles;

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Test";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.settings";
        d.version = "1.0.0";
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    std::vector<SettingsSection> settings_sections() override {
        std::vector<SettingsSection> out;
        for (const auto& title : section_titles)
            out.push_back({title, std::make_unique<pulp::view::Label>(title)});
        return out;
    }
};

std::unique_ptr<pulp::view::View> label(const std::string& t) {
    return std::make_unique<pulp::view::Label>(t);
}

}  // namespace

TEST_CASE("Processor::settings_sections defaults to none", "[format][settings]") {
    struct Bare : TestProcessor {};
    Bare p;
    REQUIRE(p.settings_sections().empty());
}

TEST_CASE("A plugin contributes named settings sections with views", "[format][settings]") {
    TestProcessor p;
    p.section_titles = {"Models", "About"};
    auto secs = p.settings_sections();
    REQUIRE(secs.size() == 2);
    REQUIRE(secs[0].title == "Models");
    REQUIRE(secs[0].view != nullptr);
    REQUIRE(secs[1].title == "About");
    REQUIRE(secs[1].view != nullptr);
}

TEST_CASE("SettingsPanel starts with host-owned Audio + MIDI tabs", "[format][settings]") {
    SettingsPanel panel;
    REQUIRE(panel.tab_count() == 2);  // Audio + MIDI
}

TEST_CASE("add_section appends plugin tabs after Audio/MIDI", "[format][settings]") {
    SettingsPanel panel;
    panel.add_section("Models", label("models"));
    REQUIRE(panel.tab_count() == 3);
    panel.add_section("License", label("license"));
    REQUIRE(panel.tab_count() == 4);
}

TEST_CASE("add_section ignores a null view", "[format][settings]") {
    SettingsPanel panel;
    const int before = panel.tab_count();
    panel.add_section("Nope", nullptr);
    REQUIRE(panel.tab_count() == before);
}

TEST_CASE("Host composition: plugin sections compose onto the host panel", "[format][settings]") {
    // Mirrors exactly what the standalone chrome does with processor.settings_sections().
    TestProcessor p;
    p.section_titles = {"Models", "License"};

    SettingsPanel panel;
    for (auto& sec : p.settings_sections())
        if (sec.view) panel.add_section(std::move(sec.title), std::move(sec.view));

    REQUIRE(panel.tab_count() == 4);  // Audio + MIDI + Models + License
}
