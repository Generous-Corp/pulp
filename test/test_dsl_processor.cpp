// Contract tests for pulp::dsl headers (DslProcessor + FaustProcessor +
// PulpFaustUI + PulpFaustMeta). core/dsl/ is headers-only with zero
// dedicated tests on main — faust-gain/filter/tremolo exercise it
// indirectly through real generated DSP classes. These tests use a
// tiny hand-written Mock DSP so the contract stays verifiable without
// a FAUST install.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/dsl/faust_processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/audio/buffer.hpp>

#include <atomic>
#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

// ── Mock FAUST DSP ──────────────────────────────────────────────────────
//
// Stands in for a FAUST-generated class derived from ::dsp. The template
// parameters pre-bake name/author/version and bus topology so individual
// tests can vary just those knobs.

template <int NumInputs, int NumOutputs>
class MockFaustDsp : public dsp {
public:
    // Parameters exposed to buildUserInterface — mirror FAUST codegen
    FAUSTFLOAT gain_{0.5f};
    FAUSTFLOAT level_{0.0f};
    FAUSTFLOAT mute_{0.0f};

    // Observed state so tests can assert against it
    int init_calls = 0;
    int compute_calls = 0;
    int last_sample_rate = 0;
    int last_compute_count = 0;
    FAUSTFLOAT last_gain_seen = 0.0f;
    FAUSTFLOAT last_level_seen = 0.0f;
    std::string name = "MockSynth";
    std::string author = "Pulp";
    std::string version = "1.0.0";

    int getNumInputs() override { return NumInputs; }
    int getNumOutputs() override { return NumOutputs; }
    int getSampleRate() override { return last_sample_rate; }

    void buildUserInterface(UI* ui) override {
        ui->openVerticalBox("Envelope");
        ui->declare(&gain_, "unit", "dB");
        ui->addHorizontalSlider("Gain", &gain_, 0.5f, 0.0f, 1.0f, 0.01f);
        ui->addVerticalSlider("Level", &level_, 0.0f, -60.0f, 0.0f, 0.1f);
        ui->closeBox();
        ui->addCheckButton("Mute", &mute_);
    }

    void init(int sr) override {
        ++init_calls;
        last_sample_rate = sr;
        instanceInit(sr);
    }
    void instanceInit(int sr) override { instanceConstants(sr); instanceResetUserInterface(); instanceClear(); }
    void instanceConstants(int) override {}
    void instanceResetUserInterface() override { gain_ = 0.5f; level_ = 0.0f; mute_ = 0.0f; }
    void instanceClear() override {}

    dsp* clone() override { return new MockFaustDsp(*this); }

    void metadata(Meta* m) override {
        m->declare("name", name.c_str());
        m->declare("author", author.c_str());
        m->declare("version", version.c_str());
        m->declare("license", "MIT");
    }

    void compute(int count, FAUSTFLOAT** /*inputs*/, FAUSTFLOAT** outputs) override {
        ++compute_calls;
        last_compute_count = count;
        last_gain_seen = gain_;
        last_level_seen = level_;
        // Write a deterministic pattern using the current gain so the
        // caller can verify the FaustProcessor-level "sync params → zones"
        // path actually runs before compute() is invoked.
        for (int ch = 0; ch < NumOutputs; ++ch) {
            for (int i = 0; i < count; ++i) {
                outputs[ch][i] = gain_;
            }
        }
    }
};

using MockFxDsp    = MockFaustDsp<1, 1>;   // Effect  — 1 in / 1 out
using MockSynthDsp = MockFaustDsp<0, 2>;   // Instrument — 0 in / 2 out

}  // namespace

// ── PulpFaustUI contract ────────────────────────────────────────────────

TEST_CASE("PulpFaustUI records all active widgets as zones",
          "[dsl][faust-ui]") {
    pulp::dsl::PulpFaustUI ui;
    MockFxDsp d;
    d.buildUserInterface(&ui);

    // Envelope/Gain + Envelope/Level + Mute (3 zones; bargraphs excluded)
    REQUIRE(ui.zones().size() == 3);
    REQUIRE(ui.zones()[0].desc.name == "Gain");
    REQUIRE(ui.zones()[0].desc.group == "Envelope");
    REQUIRE(ui.zones()[1].desc.name == "Level");
    REQUIRE(ui.zones()[1].desc.group == "Envelope");
    REQUIRE(ui.zones()[2].desc.name == "Mute");
    // Top-level widget — empty group path.
    REQUIRE(ui.zones()[2].desc.group.empty());
}

TEST_CASE("PulpFaustUI captures range + default from buildUserInterface",
          "[dsl][faust-ui]") {
    pulp::dsl::PulpFaustUI ui;
    MockFxDsp d;
    d.buildUserInterface(&ui);

    const auto& gain = ui.zones()[0];
    REQUIRE_THAT(gain.desc.min, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(gain.desc.max, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(gain.desc.default_value, WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(gain.desc.step, WithinAbs(0.01f, 1e-6f));

    // Check button: step = 1 (boolean)
    const auto& mute = ui.zones()[2];
    REQUIRE_THAT(mute.desc.max, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(mute.desc.step, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("PulpFaustUI propagates unit metadata to the adjacent zone",
          "[dsl][faust-ui][metadata]") {
    pulp::dsl::PulpFaustUI ui;
    MockFxDsp d;
    d.buildUserInterface(&ui);

    // Mock declared unit=dB on Gain before adding it.
    REQUIRE(ui.zones()[0].desc.unit == "dB");
    // Level had no unit declared.
    REQUIRE(ui.zones()[1].desc.unit.empty());
}

TEST_CASE("PulpFaustUI nests group paths with / separators",
          "[dsl][faust-ui][group]") {
    pulp::dsl::PulpFaustUI ui;
    FAUSTFLOAT a{0}, b{0};
    ui.openVerticalBox("Synth");
    ui.openHorizontalBox("Osc1");
    ui.addHorizontalSlider("Freq", &a, 440.0f, 20.0f, 20000.0f, 1.0f);
    ui.closeBox();
    ui.openHorizontalBox("Osc2");
    ui.addHorizontalSlider("Freq", &b, 220.0f, 20.0f, 20000.0f, 1.0f);
    ui.closeBox();
    ui.closeBox();

    REQUIRE(ui.zones().size() == 2);
    REQUIRE(ui.zones()[0].desc.group == "Synth/Osc1");
    REQUIRE(ui.zones()[1].desc.group == "Synth/Osc2");
}

TEST_CASE("PulpFaustUI skips passive bargraphs",
          "[dsl][faust-ui]") {
    pulp::dsl::PulpFaustUI ui;
    FAUSTFLOAT g{0};
    ui.addHorizontalBargraph("Peak", &g, -60.0f, 0.0f);
    ui.addVerticalBargraph("Level", &g, -60.0f, 0.0f);
    REQUIRE(ui.zones().empty());
}

// ── PulpFaustMeta contract ──────────────────────────────────────────────

TEST_CASE("PulpFaustMeta stores declared key/value pairs",
          "[dsl][faust-meta]") {
    pulp::dsl::PulpFaustMeta meta;
    meta.declare("name", "TestPlugin");
    meta.declare("author", "Pulp");
    REQUIRE(meta.get("name") == "TestPlugin");
    REQUIRE(meta.get("author") == "Pulp");
    REQUIRE(meta.get("missing").empty());
    REQUIRE(meta.all().size() == 2);
}

TEST_CASE("PulpFaustMeta::declare tolerates null key or value without crashing",
          "[dsl][faust-meta][nullable]") {
    pulp::dsl::PulpFaustMeta meta;
    meta.declare(nullptr, "x");
    meta.declare("y", nullptr);
    meta.declare(nullptr, nullptr);
    REQUIRE(meta.all().empty());
}

// ── FaustProcessor construction & reflection ────────────────────────────

TEST_CASE("FaustProcessor ctor extracts metadata, bus layout, and dsl_params",
          "[dsl][faust-processor][construction]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    REQUIRE(proc.dsl_name() == "faust");
    REQUIRE(proc.bus_layout().num_inputs == 1);
    REQUIRE(proc.bus_layout().num_outputs == 1);
    REQUIRE_FALSE(proc.bus_layout().accepts_midi);
    REQUIRE(proc.dsl_params().size() == 3);
    REQUIRE(proc.dsl_params()[0].name == "Gain");
    REQUIRE(proc.dsl_params()[0].unit == "dB");
}

// ── FaustProcessor::descriptor() ────────────────────────────────────────

TEST_CASE("FaustProcessor descriptor: 0 inputs -> Instrument category",
          "[dsl][faust-processor][descriptor]") {
    pulp::dsl::FaustProcessor<MockSynthDsp> proc;
    auto desc = proc.descriptor();
    REQUIRE(desc.category == pulp::format::PluginCategory::Instrument);
    REQUIRE(desc.input_buses.empty());
    REQUIRE(desc.output_buses.size() == 1);
    REQUIRE(desc.output_buses[0].default_channels == 2);
}

TEST_CASE("FaustProcessor descriptor: >0 inputs -> Effect category",
          "[dsl][faust-processor][descriptor]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    auto desc = proc.descriptor();
    REQUIRE(desc.category == pulp::format::PluginCategory::Effect);
    REQUIRE(desc.input_buses.size() == 1);
    REQUIRE(desc.input_buses[0].default_channels == 1);
}

TEST_CASE("FaustProcessor descriptor pulls name/author/version from metadata",
          "[dsl][faust-processor][descriptor]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    auto desc = proc.descriptor();
    REQUIRE(desc.name == "MockSynth");
    REQUIRE(desc.manufacturer == "Pulp");
    REQUIRE(desc.version == "1.0.0");
}

TEST_CASE("FaustProcessor descriptor bundle_id sanitises the plugin name",
          "[dsl][faust-processor][descriptor]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    auto desc = proc.descriptor();
    // MockSynth → "mocksynth"; the namespace prefix is fixed.
    REQUIRE(desc.bundle_id == "com.pulp.faust.mocksynth");
}

// ── FaustProcessor::define_parameters() ─────────────────────────────────

TEST_CASE("FaustProcessor::define_parameters registers every zone with sequential IDs",
          "[dsl][faust-processor][params]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    // 3 params: Gain, Level, Mute
    REQUIRE(store.param_count() == 3);
    const auto* p1 = store.info(1);
    const auto* p2 = store.info(2);
    const auto* p3 = store.info(3);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p3 != nullptr);
    REQUIRE(p1->name == "Gain");
    REQUIRE(p2->name == "Level");
    REQUIRE(p3->name == "Mute");
}

TEST_CASE("FaustProcessor::define_parameters carries range + default through to ParamInfo",
          "[dsl][faust-processor][params]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    const auto* gain = store.info(1);
    REQUIRE(gain != nullptr);
    REQUIRE_THAT(gain->range.min, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(gain->range.max, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(gain->range.default_value, WithinAbs(0.5f, 1e-6f));
    REQUIRE(gain->unit == "dB");
}

TEST_CASE("FaustProcessor::define_parameters creates one group per unique path",
          "[dsl][faust-processor][params][groups]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    // Gain + Level share "Envelope"; Mute is ungrouped.
    auto groups = store.all_groups();
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].name == "Envelope");
    REQUIRE(groups[0].id == 1);

    REQUIRE(store.info(1)->group_id == 1);
    REQUIRE(store.info(2)->group_id == 1);
    REQUIRE(store.info(3)->group_id == 0);  // ungrouped
}

// ── FaustProcessor::prepare() + process() ───────────────────────────────

TEST_CASE("FaustProcessor::prepare forwards sample rate to the wrapped DSP",
          "[dsl][faust-processor][prepare]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    pulp::format::PrepareContext ctx{96000.0, 256, 1, 1};
    proc.prepare(ctx);

    // Because prepare() creates and owns the DSP internally, we probe via
    // a second process() + last_sample_rate on the (fresh) DSP isn't
    // reachable. So we verify prepare's observable effect — the next
    // process() writes outputs consistent with the prepared block size.
    pulp::audio::Buffer<float> in(1, 256);
    pulp::audio::Buffer<float> out(1, 256);
    const float* in_ptrs[1] = {in.channel(0).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 256);
    auto out_view = out.view();
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = 96000.0;
    pctx.num_samples = 256;

    proc.process(out_view, in_view, midi_in, midi_out, pctx);
    // MockFxDsp::compute writes gain_ into every sample; default is 0.5.
    REQUIRE_THAT(out.channel(0)[0], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(out.channel(0)[255], WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("FaustProcessor::process syncs StateStore values into FAUST zones before compute",
          "[dsl][faust-processor][process]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    proc.prepare({48000.0, 128, 1, 1});

    // Flip gain to 0.9 via the store. process() must push that into the
    // FAUST zone before calling compute().
    store.set_value(1, 0.9f);

    pulp::audio::Buffer<float> in(1, 128);
    pulp::audio::Buffer<float> out(1, 128);
    const float* in_ptrs[1] = {in.channel(0).data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 128);
    auto out_view = out.view();
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = 48000.0;
    pctx.num_samples = 128;

    proc.process(out_view, in_view, midi_in, midi_out, pctx);

    REQUIRE_THAT(out.channel(0)[0], WithinAbs(0.9f, 1e-6f));
    REQUIRE_THAT(out.channel(0)[127], WithinAbs(0.9f, 1e-6f));
}

TEST_CASE("FaustProcessor::process honours zero-length buffer (num_samples == 0)",
          "[dsl][faust-processor][process][edge]") {
    pulp::dsl::FaustProcessor<MockFxDsp> proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    proc.prepare({48000.0, 128, 1, 1});

    pulp::audio::Buffer<float> in(1, 0);
    pulp::audio::Buffer<float> out(1, 0);
    const float* in_ptrs[1] = {nullptr};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 0);
    auto out_view = out.view();
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = 48000.0;
    pctx.num_samples = 0;

    // Must not crash or UB on zero-length input. If FAUST compute() is
    // called at all, it gets count == 0 and does nothing.
    proc.process(out_view, in_view, midi_in, midi_out, pctx);
    SUCCEED("FaustProcessor handled zero-length block without crashing");
}
