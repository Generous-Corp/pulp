// Regression: StandaloneApp::apply_config must NOT recreate the processor while
// the editor is open, or an editor ViewBridge holding a Processor& dangles
// (use-after-free, #2693). apply_config now does a soft restart that tears down
// only the audio/MIDI device and re-prepare()s the *same* processor instance.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/standalone.hpp>
#include <pulp/format/processor.hpp>

#include <memory>

using namespace pulp::format;

namespace {

class ApplyConfigProc : public Processor {
public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "ApplyConfigProc";
        d.manufacturer = "Pulp";
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

int g_factory_calls = 0;
std::unique_ptr<Processor> make_apply_config_proc() {
    ++g_factory_calls;
    return std::make_unique<ApplyConfigProc>();
}

}  // namespace

TEST_CASE("StandaloneApp::apply_config preserves the processor instance",
          "[format][standalone][issue-2693]") {
    g_factory_calls = 0;
    StandaloneApp app(&make_apply_config_proc);

    StandaloneConfig cfg;
    cfg.headless = true;
    cfg.output_channels = 2;
    cfg.buffer_size = 256;
    app.set_config(cfg);

    if (!app.start()) {
        // No usable audio device (e.g. headless Linux CI). The running-path
        // soft-restart is exercised on hosts with an audio device (macOS CI).
        SUCCEED("no audio device available — running-path check skipped");
        return;
    }

    Processor* before = app.processor();
    REQUIRE(before != nullptr);
    const int calls_after_start = g_factory_calls;

    StandaloneConfig cfg2 = cfg;
    cfg2.buffer_size = 512;  // a device-level change that triggers a restart
    REQUIRE(app.apply_config(cfg2));

    // The same Processor instance must survive the settings-apply: a recreate
    // would dangle an editor ViewBridge's Processor& (#2693).
    REQUIRE(app.processor() == before);
    REQUIRE(g_factory_calls == calls_after_start);

    app.stop();
}
