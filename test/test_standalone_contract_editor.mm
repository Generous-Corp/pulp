// test_standalone_contract_editor.mm — the standalone app opening a
// processor's editor through the editor-host contract (headless).
//
// Every run is a screenshot-only launch: the window is never shown and no
// audio device is created (StandaloneConfig::screenshot_path with headless).
// The editor records every contract call and reports a fixed one-pixel PNG as
// its captured frame.

#include <catch2/catch_test_macros.hpp>

#include "support/recording_contract_editor.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/state/store.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using pulp::test_support::contract_config;
using pulp::test_support::contract_log;
using pulp::test_support::EditorEnvironment;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

namespace {

constexpr pulp::state::ParamID kGain = 1;

class ContractProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Contract Standalone";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.standalone-contract-editor";
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

std::vector<unsigned char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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

TEST_CASE("the standalone app hosts a contract editor headless and captures its frame",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-contract");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    // Space is the standalone's transport key; "a" is the editor's.
    config.test_key_sequence = "space,a";
    config.test_key_frame_delay = 2;
    app.set_config(config);
    REQUIRE(app.run_with_editor(false));

    // A screenshot-only launch: no audio device was created.
    REQUIRE(app.audio_skipped_for_capture());

    // The editor ran in the window, was ticked, captured, then detached.
    REQUIRE(contract_log().saw("attach:cocoa_view"));
    REQUIRE(contract_log().idles > 0);
    REQUIRE(contract_log().saw("capture"));
    REQUIRE(contract_log().saw("detach"));
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);

    // The captured frame is what the editor produced.
    REQUIRE(read_file(png) ==
            std::vector<unsigned char>(pulp::test_support::recorded_frame_png().begin(),
                                       pulp::test_support::recorded_frame_png().end()));
    std::filesystem::remove(png);

    // Space was offered, forwarded, and reached the transport; "a" was the
    // editor's and stopped there.
    REQUIRE(contract_log().saw("offer:space"));
    REQUIRE(contract_log().saw("offer:char"));
    // The transport starts playing; one Space stopped it.
    REQUIRE_FALSE(app.test_input_host().commanded_playing());
}

TEST_CASE("a standalone contract editor follows the window's size and scale",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-contract-size");
    pulp::format::StandaloneApp app(create_contract_processor);
    app.set_config(headless_config(png));
    REQUIRE(app.run_with_editor(false));
    // The window took the editor's own size and passed its backing scale.
    REQUIRE(contract_log().last_scale >= 1.0);
    // Without a Space the transport keeps playing (the control for the
    // Space test above).
    REQUIRE(app.test_input_host().commanded_playing());
    std::filesystem::remove(png);
}

TEST_CASE("a headless standalone contract editor without frame capture fails closed",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    contract_config().captures_frames = false;
    const auto png = temp_png("pulp-standalone-contract-nocapture");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    app.set_config(headless_config(png));
    REQUIRE_FALSE(app.run_with_editor(false));
    REQUIRE_FALSE(std::filesystem::exists(png));
    // Closed in order even so.
    REQUIRE(contract_log().saw("detach"));
    REQUIRE(contract_log().live == 0);
    reset_contract_recording();
}

TEST_CASE("a standalone contract editor that will not build or attach fails the run",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    const auto png = temp_png("pulp-standalone-contract-refused");
    {
        reset_contract_recording();
        contract_config().factory_builds_editor = false;
        pulp::format::StandaloneApp app(create_contract_processor);
        app.set_config(headless_config(png));
        REQUIRE_FALSE(app.run_with_editor(false));
    }
    {
        reset_contract_recording();
        contract_config().attach_succeeds = false;
        pulp::format::StandaloneApp app(create_contract_processor);
        app.set_config(headless_config(png));
        REQUIRE_FALSE(app.run_with_editor(false));
        REQUIRE(contract_log().live == 0);
    }
    {
        reset_contract_recording();
        pulp::format::StandaloneApp app(create_contract_processor);
        auto config = headless_config(png);
        config.test_key_sequence = "not-a-key";
        app.set_config(config);
        REQUIRE_FALSE(app.run_with_editor(false));
    }
    reset_contract_recording();
    REQUIRE_FALSE(std::filesystem::exists(png));
}
