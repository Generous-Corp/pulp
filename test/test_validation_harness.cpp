// Tests for the Phase 2 validation harness.
// Covers: harness happy path, report generation, MIDI control surface,
// missing validator graceful degradation, and state round-trips.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/format/validation_harness.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

// ── Test Processor ──────────────────────────────────────────────────────────

namespace {

static class TestGainProcessor* last_processor = nullptr;

class TestGainProcessor : public pulp::format::Processor {
public:
    TestGainProcessor() { last_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "HarnessTestGain",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.harness-gain",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = 2,
            .name = "Pan",
            .unit = "",
            .range = {-1.0f, 1.0f, 0.0f, 0.01f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(
        pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input,
        pulp::midi::MidiBuffer& midi_in, pulp::midi::MidiBuffer&,
        const pulp::format::ProcessContext&) override
    {
        float db = state().get_value(1);
        float gain = std::pow(10.0f, db / 20.0f);

        // Count MIDI events for testing
        for (const auto& ev : midi_in) {
            if (ev.is_note_on()) ++note_on_count_;
        }

        for (std::size_t ch = 0; ch < output.num_channels() && ch < input.num_channels(); ++ch) {
            auto in = input.channel(ch);
            auto out = output.channel(ch);
            for (std::size_t i = 0; i < output.num_samples(); ++i)
                out[i] = in[i] * gain;
        }
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    int note_on_count_ = 0;
    std::string plugin_state;
};

std::unique_ptr<pulp::format::Processor> create_test_gain() {
    return std::make_unique<TestGainProcessor>();
}

std::filesystem::path make_temp_path(const char* stem) {
    auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / (std::string(stem) + "-" + unique + ".json");
}

} // anonymous namespace

using Catch::Matchers::WithinAbs;
using Catch::Matchers::ContainsSubstring;

// ── Harness construction and basic control ──────────────────────────────────

TEST_CASE("ValidationHarness creates and reports descriptor", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    REQUIRE(harness.descriptor().name == "HarnessTestGain");
}

TEST_CASE("ValidationHarness set/get param", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.set_param(1, -6.0f);
    REQUIRE_THAT(harness.get_param(1), WithinAbs(-6.0, 0.01));

    harness.set_param(2, 0.5f);
    REQUIRE_THAT(harness.get_param(2), WithinAbs(0.5, 0.01));
}

TEST_CASE("ValidationHarness processes silence blocks", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.buffer_size = 64});
    harness.prepare();

    auto output = harness.process_blocks(1);
    // 2 channels * 64 samples = 128 interleaved floats
    REQUIRE(output.size() == 128);

    // All zeros (silence in, unity gain)
    for (float s : output) {
        REQUIRE_THAT(static_cast<double>(s), WithinAbs(0.0, 0.0001));
    }
}

TEST_CASE("ValidationHarness processes audio buffer with gain", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.buffer_size = 4, .input_channels = 2, .output_channels = 2});
    harness.prepare();

    harness.set_param(1, 6.0f); // +6 dB ~ 2x

    // Create interleaved input: 0.5 on both channels
    std::vector<float> input(8, 0.5f); // 4 samples * 2 channels
    auto output = harness.process_buffer(input, 2, 4);

    REQUIRE(output.size() == 8);
    float expected = 0.5f * std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(static_cast<double>(output[0]),
                 Catch::Matchers::WithinRel(static_cast<double>(expected), 0.01));
}

TEST_CASE("ValidationHarness MIDI note-on queuing", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.buffer_size = 64});
    harness.prepare();

    harness.send_midi_note_on(0, 60, 100);
    harness.send_midi_note_on(0, 64, 80);

    // Process to flush MIDI
    harness.process_blocks(1);

    // MIDI was consumed (no crash, no assertion failure)
    // The processor counts note-ons internally
    SUCCEED("MIDI events processed without error");
}

TEST_CASE("ValidationHarness state round-trip", "[harness][phase2]") {
    pulp::format::ValidationHarness h1(create_test_gain);
    h1.configure({});
    h1.set_param(1, -12.5f);
    h1.set_param(2, 0.75f);

    auto saved = h1.save_state();
    REQUIRE_FALSE(saved.empty());

    pulp::format::ValidationHarness h2(create_test_gain);
    h2.configure({});
    REQUIRE(h2.load_state(saved));
    REQUIRE_THAT(h2.get_param(1), WithinAbs(-12.5, 0.01));
    REQUIRE_THAT(h2.get_param(2), WithinAbs(0.75, 0.01));
}

TEST_CASE("ValidationHarness state round-trip includes plugin-owned payload", "[harness][phase2]") {
    pulp::format::ValidationHarness h1(create_test_gain);
    h1.configure({});
    REQUIRE(last_processor != nullptr);
    last_processor->plugin_state = "snapshots=A|B";
    h1.set_param(1, -9.0f);

    auto saved = h1.save_state();

    pulp::format::ValidationHarness h2(create_test_gain);
    h2.configure({});
    auto* restored_processor = last_processor;
    REQUIRE(restored_processor != nullptr);
    restored_processor->plugin_state = "stale";

    REQUIRE(h2.load_state(saved));
    REQUIRE_THAT(h2.get_param(1), WithinAbs(-9.0, 0.01));
    REQUIRE(restored_processor->plugin_state == "snapshots=A|B");
}

// ── Report generation ───────────────────────────────────────────────────────

TEST_CASE("ValidationHarness generates valid report JSON", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.git_ref = "abc1234", .run_id = "test-run-1"});

    // Add a manual entry
    pulp::format::ReportEntry entry;
    entry.type = "test_suite";
    entry.status = pulp::format::ValidationStatus::pass;
    entry.target = "unit-tests";
    entry.duration_ms = 1234.5;
    entry.payload_json = "{\"total\": 50, \"passed\": 50, \"failed\": 0}";
    harness.add_entry(entry);

    auto report = harness.generate_report();

    // Verify JSON structure
    REQUIRE_THAT(report, ContainsSubstring("\"version\": 1"));
    REQUIRE_THAT(report, ContainsSubstring("\"timestamp\""));
    REQUIRE_THAT(report, ContainsSubstring("\"reports\""));
    REQUIRE_THAT(report, ContainsSubstring("\"test_suite\""));
    REQUIRE_THAT(report, ContainsSubstring("\"pass\""));
    REQUIRE_THAT(report, ContainsSubstring("\"abc1234\""));
    REQUIRE_THAT(report, ContainsSubstring("\"test-run-1\""));
    REQUIRE_THAT(report, ContainsSubstring("\"total\": 50"));
}

TEST_CASE("ValidationHarness empty report is valid", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto report = harness.generate_report();
    REQUIRE_THAT(report, ContainsSubstring("\"version\": 1"));
    REQUIRE_THAT(report, ContainsSubstring("\"reports\": [\n  ]"));
}

TEST_CASE("ValidationHarness write_report creates file", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    pulp::format::ReportEntry entry;
    entry.type = "validator";
    entry.status = pulp::format::ValidationStatus::skip;
    entry.target = "test-plugin";
    entry.error_message = "pluginval not found";
    harness.add_entry(entry);

    auto tmp = make_temp_path("pulp-harness-test-report");
    REQUIRE(harness.write_report(tmp));
    REQUIRE(std::filesystem::exists(tmp));

    std::string content;
    {
        std::ifstream f(tmp);
        content.assign(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
    }
    REQUIRE_THAT(content, ContainsSubstring("\"version\": 1"));
    REQUIRE_THAT(content, ContainsSubstring("\"skip\""));

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    REQUIRE_FALSE(ec);
}

// ── Validator graceful degradation ──────────────────────────────────────────

TEST_CASE("ValidationHarness run_validator skips missing tool", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    // Create a dummy plugin file so the file-existence check passes
    auto tmp_plugin = std::filesystem::temp_directory_path() / "fake-plugin-for-skip-test.vst3";
    { std::ofstream f(tmp_plugin); f << "dummy"; }

    // Use a tool name that definitely won't exist
    auto entry = harness.run_validator("nonexistent-validator-tool-xyz", tmp_plugin);

    REQUIRE(entry.status == pulp::format::ValidationStatus::skip);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("not found"));

    std::filesystem::remove(tmp_plugin);
}

TEST_CASE("ValidationHarness run_validator errors on missing plugin", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto entry = harness.run_validator("pluginval",
                                        "/tmp/this-plugin-does-not-exist.vst3");

    REQUIRE(entry.status == pulp::format::ValidationStatus::error);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("not found"));
}

// ── Screenshot / inspector providers (#298) ─────────────────────────────────

TEST_CASE("ValidationHarness screenshot skips without provider",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    // No provider installed — must report skip, never pass.
    auto entry = harness.capture_screenshot("/tmp/test-unprovided.png");
    REQUIRE(entry.status == pulp::format::ValidationStatus::skip);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("provider"));
    REQUIRE_FALSE(entry.payload_json.empty());
}

TEST_CASE("ValidationHarness screenshot passes when provider returns true",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    std::filesystem::path captured_path;
    uint32_t captured_w = 0, captured_h = 0;
    harness.set_capture_screenshot_provider(
        [&](const std::filesystem::path& p,
            uint32_t w, uint32_t h, float, const std::string&) {
            captured_path = p;
            captured_w = w;
            captured_h = h;
            // Simulate a successful write.
            std::ofstream(p) << "PNG-STUB";
            return true;
        });

    auto entry = harness.capture_screenshot("/tmp/test-provided.png");
    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE(captured_path == std::filesystem::path("/tmp/test-provided.png"));
    REQUIRE(captured_w > 0);
    REQUIRE(captured_h > 0);
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"view_id\": \"provider\""));
    std::filesystem::remove("/tmp/test-provided.png");
}

TEST_CASE("ValidationHarness screenshot fails when provider returns false",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.set_capture_screenshot_provider(
        [](const std::filesystem::path&, uint32_t, uint32_t, float, const std::string&) {
            return false;  // simulate capture failure
        });

    auto entry = harness.capture_screenshot("/tmp/test-fail.png");
    REQUIRE(entry.status == pulp::format::ValidationStatus::fail);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("returned false"));
}

TEST_CASE("ValidationHarness inspector skips without provider",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto entry = harness.capture_inspector();
    REQUIRE(entry.status == pulp::format::ValidationStatus::skip);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("provider"));
}

TEST_CASE("ValidationHarness inspector passes with provider",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.set_capture_inspector_provider([]() {
        return std::string("{\"type\":\"View\",\"children\":[]}");
    });

    auto entry = harness.capture_inspector();
    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"children\""));
}

TEST_CASE("ValidationHarness compare_screenshots identical files -> pass",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto a = std::filesystem::temp_directory_path() / "harness-ref.bin";
    auto b = std::filesystem::temp_directory_path() / "harness-ren.bin";
    std::ofstream(a) << "byte-identical-content";
    std::ofstream(b) << "byte-identical-content";

    auto entry = harness.compare_screenshots(a, b);
    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"similarity\": 1"));
    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

TEST_CASE("ValidationHarness compare_screenshots differing files -> fail",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto a = std::filesystem::temp_directory_path() / "harness-ref2.bin";
    auto b = std::filesystem::temp_directory_path() / "harness-ren2.bin";
    std::ofstream(a) << "content-A";
    std::ofstream(b) << "content-B";

    auto entry = harness.compare_screenshots(a, b);
    REQUIRE(entry.status == pulp::format::ValidationStatus::fail);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("differ"));
    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

// #308 Codex P1: compare_screenshots must NOT throw filesystem_error
// when a caller passes a directory or other non-regular path. It must
// return a ValidationStatus::error entry so the harness's report-
// first behavior is preserved under bad input.
TEST_CASE("ValidationHarness compare_screenshots directory input -> error, not throw",
          "[harness][phase2][issue-308]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = std::filesystem::temp_directory_path() / "harness-dir-input";
    std::filesystem::create_directories(dir);
    auto file = std::filesystem::temp_directory_path() / "harness-file-input.bin";
    std::ofstream(file) << "some content";

    // reference is a directory — must error, not throw.
    auto entry = harness.compare_screenshots(dir, file);
    REQUIRE(entry.status == pulp::format::ValidationStatus::error);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("regular file"));

    // rendered is a directory — must error, not throw.
    entry = harness.compare_screenshots(file, dir);
    REQUIRE(entry.status == pulp::format::ValidationStatus::error);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("regular file"));

    std::filesystem::remove_all(dir);
    std::filesystem::remove(file);
}

// ── Clear entries ───────────────────────────────────────────────────────────

TEST_CASE("ValidationHarness clear_entries resets accumulator", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.capture_screenshot("/tmp/test.png");
    harness.capture_inspector();
    REQUIRE(harness.entries().size() == 2);

    harness.clear_entries();
    REQUIRE(harness.entries().empty());
}

// ── to_string ───────────────────────────────────────────────────────────────

TEST_CASE("ValidationStatus to_string covers all values", "[harness][phase2]") {
    using pulp::format::ValidationStatus;
    using pulp::format::to_string;

    REQUIRE(std::string(to_string(ValidationStatus::pass)) == "pass");
    REQUIRE(std::string(to_string(ValidationStatus::fail)) == "fail");
    REQUIRE(std::string(to_string(ValidationStatus::skip)) == "skip");
    REQUIRE(std::string(to_string(ValidationStatus::error)) == "error");
}
