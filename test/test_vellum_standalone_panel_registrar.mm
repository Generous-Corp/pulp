// The view-free standalone with the generated panel registrar linked in:
// launching headless builds the Vellum musical typing panel, because the
// standalone calls install_standalone_panels() itself.
//
// Every run is a screenshot-only launch: the window is never shown and no
// audio device is created.

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

#include "support/recording_contract_editor.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/standalone_panel.hpp>
#include <pulp/state/store.hpp>
#include <pulp/vellum_audio/standalone_typing_panel.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using pulp::test_support::EditorEnvironment;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

namespace {

constexpr pulp::state::ParamID kGain = 1;

class ContractProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Registrar Standalone";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.vellum-standalone-panel-registrar";
        d.version = "1.0.0";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo info;
        info.id = kGain;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
    pulp::format::EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    RecordingContractFactory factory_;
};

std::unique_ptr<pulp::format::Processor> create_contract_processor() {
    return std::make_unique<ContractProcessor>();
}

std::string temp_png(const char* name) {
    const auto dir = std::filesystem::temp_directory_path();
    return (dir / (std::string(name) + "-" + std::to_string(::getpid()) + ".png")).string();
}

pulp::format::StandaloneConfig headless_config(const std::string& png) {
    pulp::format::StandaloneConfig config;
    config.headless = true;
    config.screenshot_path = png;
    config.screenshot_frame_delay = 3;
    config.persist_settings = false;
    config.show_settings_tab = false;
    return config;
}

} // namespace

TEST_CASE("the generated registrar gives the standalone its Vellum typing panel",
          "[standalone][standalone-contract][standalone-panel][vellum-typing]") {
    EditorEnvironment env;
    reset_contract_recording();
    pulp::format::detail::reset_standalone_panel_factories_for_testing();
    const auto png = temp_png("pulp-vellum-standalone-registrar");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    config.enable_musical_typing_keyboard = true;
    std::size_t panels_in_window = 0;
    config.test_on_editor_window = [&app, &panels_in_window](void*) {
        panels_in_window = app.panel_count();
    };
    app.set_config(config);
    const bool ran = app.run_with_editor(false);
    pulp::format::detail::reset_standalone_panel_factories_for_testing();
    std::filesystem::remove(png);
    REQUIRE(ran);
    CHECK(panels_in_window == 1);
}

TEST_CASE("the registrar's typing panel stays out unless the config asks for it",
          "[standalone][standalone-contract][standalone-panel][vellum-typing]") {
    EditorEnvironment env;
    reset_contract_recording();
    pulp::format::detail::reset_standalone_panel_factories_for_testing();
    const auto png = temp_png("pulp-vellum-standalone-registrar-off");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    std::size_t panels_in_window = 99;
    config.test_on_editor_window = [&app, &panels_in_window](void*) {
        panels_in_window = app.panel_count();
    };
    app.set_config(config);
    const bool ran = app.run_with_editor(false);
    pulp::format::detail::reset_standalone_panel_factories_for_testing();
    std::filesystem::remove(png);
    REQUIRE(ran);
    CHECK(panels_in_window == 0);
}
