#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/format/web/wam_adapter.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

using namespace pulp::format;
using namespace pulp::format::wam;

// Minimal test processor
namespace {
enum Params { kGain = 0, kBypass = 1 };

class TestWamProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override {
        return {
            .name = "TestWAM",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulptest.wam",
            .version = "1.0.0",
            .category = PluginCategory::Effect,
            .accepts_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kGain,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = kBypass,
            .name = "Bypass",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out_ch = output.channel(ch);
            auto in_ch = (ch < input.num_channels()) ? input.channel(ch)
                : std::span<const float>{};
            for (std::size_t i = 0; i < out_ch.size(); ++i) {
                float in_val = (i < in_ch.size()) ? in_ch[i] : 0.0f;
                out_ch[i] = in_val * 0.5f; // simple gain
            }
        }
    }
};

std::unique_ptr<Processor> create_test_wam() {
    return std::make_unique<TestWamProcessor>();
}

std::unique_ptr<Processor> create_null_wam() {
    return {};
}
}

// ── WAMv2 Descriptor Tests ──────────────────────────────────────────────

TEST_CASE("WamDescriptorData from processor", "[format][wam]") {
    PluginDescriptor desc;
    desc.name = "TestPlugin";
    desc.manufacturer = "TestCo";
    desc.version = "2.0.0";
    desc.category = PluginCategory::Instrument;
    desc.accepts_midi = true;
    desc.produces_midi = false;

    auto wam_desc = WamDescriptorData::from_processor(desc);

    REQUIRE(wam_desc.name == "TestPlugin");
    REQUIRE(wam_desc.vendor == "TestCo");
    REQUIRE(wam_desc.version == "2.0.0");
    REQUIRE(wam_desc.is_instrument == true);
    REQUIRE(wam_desc.has_midi_input == true);
    REQUIRE(wam_desc.has_midi_output == false);
    REQUIRE(wam_desc.has_automation_input == true);
    REQUIRE(wam_desc.api_version == "2.0.0");
}

TEST_CASE("WamDescriptorData to_json", "[format][wam]") {
    WamDescriptorData desc;
    desc.name = "MyPlugin";
    desc.vendor = "MyCompany";
    desc.version = "1.0.0";
    desc.is_instrument = true;
    desc.has_midi_input = true;

    auto json = desc.to_json();
    REQUIRE(json.find("\"name\":\"MyPlugin\"") != std::string::npos);
    REQUIRE(json.find("\"isInstrument\":true") != std::string::npos);
    REQUIRE(json.find("\"hasMidiInput\":true") != std::string::npos);
    REQUIRE(json.find("\"apiVersion\":\"2.0.0\"") != std::string::npos);
}

TEST_CASE("WamDescriptorData defaults serialize every advertised capability",
          "[format][wam]") {
    WamDescriptorData desc;
    desc.name = "Default";
    auto json = desc.to_json();

    REQUIRE(json.find("\"name\":\"Default\"") != std::string::npos);
    REQUIRE(json.find("\"hasAudioInput\":true") != std::string::npos);
    REQUIRE(json.find("\"hasAudioOutput\":true") != std::string::npos);
    REQUIRE(json.find("\"hasMidiInput\":false") != std::string::npos);
    REQUIRE(json.find("\"hasMidiOutput\":false") != std::string::npos);
    REQUIRE(json.find("\"hasAutomationInput\":true") != std::string::npos);
    REQUIRE(json.find("\"hasAutomationOutput\":false") != std::string::npos);
    REQUIRE(json.find("\"hasMpeInput\":false") != std::string::npos);
    REQUIRE(json.find("\"hasMpeOutput\":false") != std::string::npos);
}

// ── WAMv2 ProcessorBridge Tests ─────────────────────────────────────────

TEST_CASE("WamProcessorBridge initialization", "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);

    REQUIRE(bridge.initialize(48000.0, 128));

    auto desc = bridge.descriptor();
    REQUIRE(desc.name == "TestWAM");
    REQUIRE(desc.vendor == "PulpTest");
}

TEST_CASE("WamProcessorBridge handles uninitialized and null-factory states",
          "[format][wam]") {
    WamProcessorBridge uninitialized(create_test_wam);
    auto empty_desc = uninitialized.descriptor();
    REQUIRE(empty_desc.name.empty());
    REQUIRE(empty_desc.vendor.empty());
    REQUIRE(uninitialized.get_parameter_info().empty());

    // Planar: 2 channels x 2 frames. An uninitialized bridge is a no-op, so
    // output stays untouched (layout-agnostic).
    float input[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float output[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    const float* inCh[2]  = {input, input + 2};
    float*       outCh[2] = {output, output + 2};
    uninitialized.process(inCh, outCh, 2, 2);
    REQUIRE(output[0] == 9.0f);
    REQUIRE(output[3] == 9.0f);

    WamProcessorBridge null_bridge(create_null_wam);
    REQUIRE_FALSE(null_bridge.initialize(48000.0, 64));
    REQUIRE(null_bridge.descriptor().name.empty());
}

TEST_CASE("WamProcessorBridge parameter info", "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);
    bridge.initialize(48000.0, 128);

    auto params = bridge.get_parameter_info();
    REQUIRE(params.size() == 2);

    REQUIRE(params[0].label == "Gain");
    REQUIRE(params[0].type == "float");
    REQUIRE(params[0].min_value == -60.0f);
    REQUIRE(params[0].max_value == 24.0f);
    REQUIRE(params[0].unit == "dB");

    REQUIRE(params[1].label == "Bypass");
    REQUIRE(params[1].type == "boolean");
}

TEST_CASE("WamProcessorBridge parameter get/set", "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);
    bridge.initialize(48000.0, 128);

    REQUIRE(bridge.get_parameter_value("0") == 0.0f);

    bridge.set_parameter_value("0", 6.0f);
    REQUIRE(bridge.get_parameter_value("0") == 6.0f);
}

TEST_CASE("WamProcessorBridge audio processing", "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);
    bridge.initialize(48000.0, 128);

    // Planar stereo: ch0 (L) = [0,4), ch1 (R) = [4,8). 4 frames.
    // L = R = {1.0, 0.5, -1.0, 0.0}.
    float input[8] = {1.0f, 0.5f, -1.0f, 0.0f,   1.0f, 0.5f, -1.0f, 0.0f};
    float output[8] = {};
    const float* inCh[2]  = {input, input + 4};
    float*       outCh[2] = {output, output + 4};

    bridge.process(inCh, outCh, 2, 4);

    // Processor applies 0.5x gain. Planar output: L=[0,4), R=[4,8).
    REQUIRE(output[0] == Catch::Approx(0.5f));   // L0 = 1.0*0.5
    REQUIRE(output[4] == Catch::Approx(0.5f));   // R0 = 1.0*0.5
    REQUIRE(output[1] == Catch::Approx(0.25f));  // L1 = 0.5*0.5
    REQUIRE(output[2] == Catch::Approx(-0.5f));  // L2 = -1.0*0.5
}

TEST_CASE("WamProcessorBridge clamps processing to prepared channel count",
          "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);
    bridge.initialize(48000.0, 128);

    // Planar 3-channel, 4 frames: ch0=[0,4), ch1=[4,8), ch2=[8,12).
    // ch2 is clamped away (bridge prepared for 2 channels).
    float input[12] = {
        1.0f,  0.5f,  -1.0f, 0.0f,    // ch0
        2.0f,  0.25f, -2.0f, 0.0f,    // ch1
        99.0f, 88.0f, 77.0f, 66.0f,   // ch2 (dropped)
    };
    float output[12] = {};
    const float* inCh[3]  = {input, input + 4, input + 8};
    float*       outCh[3] = {output, output + 4, output + 8};

    bridge.process(inCh, outCh, 3, 4);

    // 0.5x gain on the two prepared channels; ch2 left untouched (clamped).
    REQUIRE(output[0] == Catch::Approx(0.5f));    // ch0[0] = 1.0*0.5
    REQUIRE(output[4] == Catch::Approx(1.0f));    // ch1[0] = 2.0*0.5
    REQUIRE(output[8] == Catch::Approx(0.0f));    // ch2[0] clamped
    REQUIRE(output[1] == Catch::Approx(0.25f));   // ch0[1] = 0.5*0.5
    REQUIRE(output[5] == Catch::Approx(0.125f));  // ch1[1] = 0.25*0.5
    REQUIRE(output[9] == Catch::Approx(0.0f));    // ch2[1] clamped
}

TEST_CASE("WamProcessorBridge MIDI scheduling", "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);
    bridge.initialize(48000.0, 128);

    bridge.schedule_midi(0x90, 60, 127, 0);
    bridge.schedule_midi(0x80, 60, 0, 64);

    float input[4] = {};
    float output[4] = {};
    const float* inCh[2]  = {input, input + 2};
    float*       outCh[2] = {output, output + 2};
    bridge.process(inCh, outCh, 2, 2);
}

TEST_CASE("WamProcessorBridge ignores unsupported MIDI statuses",
          "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);
    bridge.initialize(48000.0, 128);

    bridge.schedule_midi(0xF0, 1, 2, 0);
    bridge.schedule_midi(0xE0, 0, 64, 1);

    float input[4] = {};
    float output[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float* inCh[2]  = {input, input + 2};
    float*       outCh[2] = {output, output + 2};
    bridge.process(inCh, outCh, 2, 2);

    REQUIRE(output[0] == Catch::Approx(0.0f));
    REQUIRE(output[3] == Catch::Approx(0.0f));
}

TEST_CASE("WamProcessorBridge state round-trip", "[format][wam]") {
    WamProcessorBridge bridge(create_test_wam);
    bridge.initialize(48000.0, 128);

    bridge.set_parameter_value("0", 12.0f);
    auto state = bridge.get_state();
    REQUIRE_FALSE(state.empty());

    bridge.set_parameter_value("0", 0.0f);
    REQUIRE(bridge.set_state(state.data(), state.size()));
    REQUIRE(bridge.get_parameter_value("0") == Catch::Approx(12.0f));
}

// ── WebCLAP Tests ───────────────────────────────────────────────────────

TEST_CASE("WCLAP memory allocation functions", "[format][wclap]") {
    void* ptr = ::malloc(64);
    REQUIRE(ptr != nullptr);
    ::free(ptr);

    void* p1 = ::malloc(32);
    void* p2 = ::realloc(p1, 64);
    REQUIRE(p2 != nullptr);
    ::free(p2);
}

TEST_CASE("WCLAP descriptor has required fields", "[format][wclap]") {
    auto proc = create_test_wam();
    auto desc = proc->descriptor();

    REQUIRE_FALSE(desc.name.empty());
    REQUIRE_FALSE(desc.manufacturer.empty());
    REQUIRE_FALSE(desc.bundle_id.empty());
    REQUIRE_FALSE(desc.version.empty());
}
