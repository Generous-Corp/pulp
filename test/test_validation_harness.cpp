// Tests for the Phase 2 validation harness.
// Covers: harness happy path, report generation, MIDI control surface,
// missing validator graceful degradation, and state round-trips.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/format/validation_harness.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

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
            else if (ev.is_note_off()) ++note_off_count_;
            else if (ev.is_cc()) ++cc_count_;
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
    int note_off_count_ = 0;
    int cc_count_ = 0;
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

std::filesystem::path make_temp_dir(const char* stem) {
    auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / (std::string(stem) + "-" + unique);
}

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* prev = std::getenv(name_.c_str())) {
            prev_ = std::string(prev);
        }
    }

    ~ScopedEnv() {
        if (prev_) {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), prev_->c_str());
#else
            ::setenv(name_.c_str(), prev_->c_str(), /*overwrite=*/1);
#endif
        } else {
#if defined(_WIN32)
            _putenv_s(name_.c_str(), "");
#else
            ::unsetenv(name_.c_str());
#endif
        }
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

private:
    std::string name_;
    std::optional<std::string> prev_;
};

#if !defined(_WIN32)
class ScopedPathPrefix {
public:
    explicit ScopedPathPrefix(const std::filesystem::path& prefix) {
        const char* existing = std::getenv("PATH");
        if (existing != nullptr) {
            had_old_path_ = true;
            old_path_ = existing;
        }

        std::string next_path = prefix.string();
        if (!old_path_.empty()) {
            next_path += ":" + old_path_;
        }
        REQUIRE(::setenv("PATH", next_path.c_str(), 1) == 0);
    }

    ~ScopedPathPrefix() {
        if (had_old_path_) {
            (void)::setenv("PATH", old_path_.c_str(), 1);
        } else {
            (void)::unsetenv("PATH");
        }
    }

private:
    bool had_old_path_ = false;
    std::string old_path_;
};

std::filesystem::path write_executable_script(
    const std::filesystem::path& dir,
    const std::string& name,
    const std::string& contents)
{
    std::filesystem::create_directories(dir);
    auto script = dir / name;
    {
        std::ofstream f(script);
        f << contents;
    }
    REQUIRE(::chmod(script.string().c_str(), 0755) == 0);
    return script;
}
#endif

} // anonymous namespace

using Catch::Matchers::WithinAbs;
using Catch::Matchers::ContainsSubstring;

// ── Harness construction and basic control ──────────────────────────────────

TEST_CASE("ValidationHarness creates and reports descriptor", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    REQUIRE(harness.descriptor().name == "HarnessTestGain");
}

TEST_CASE("ValidationHarness exposes the underlying headless host",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    const auto& const_harness = harness;

    REQUIRE(&harness.host().descriptor() == &harness.descriptor());
    REQUIRE(&const_harness.host().descriptor() == &const_harness.descriptor());
}

TEST_CASE("ValidationHarness option and entry defaults match report schema",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationRunOptions options;
    REQUIRE(options.output_dir.empty());
    REQUIRE(options.sample_rate == 48000.0);
    REQUIRE(options.buffer_size == 512);
    REQUIRE(options.input_channels == 2);
    REQUIRE(options.output_channels == 2);
    REQUIRE(options.screenshot_width == 800);
    REQUIRE(options.screenshot_height == 600);
    REQUIRE(options.screenshot_scale == 2.0f);
    REQUIRE(options.screenshot_backend == "default");
    REQUIRE(options.diff_tolerance == 32);
    REQUIRE(options.diff_threshold == 0.85f);
    REQUIRE(options.git_ref.empty());
    REQUIRE(options.run_id.empty());

    pulp::format::ReportEntry entry;
    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE(entry.type.empty());
    REQUIRE(entry.target.empty());
    REQUIRE(entry.duration_ms == 0.0);
    REQUIRE(entry.error_message.empty());
    REQUIRE(entry.payload_json.empty());
}

TEST_CASE("ValidationHarness set/get param", "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.set_param(1, -6.0f);
    REQUIRE_THAT(harness.get_param(1), WithinAbs(-6.0, 0.01));

    harness.set_param(2, 0.5f);
    REQUIRE_THAT(harness.get_param(2), WithinAbs(0.5, 0.01));
}

TEST_CASE("ValidationHarness configure creates artifact directory",
          "[harness][phase2][coverage][issue-646]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    auto out_dir = make_temp_dir("pulp-harness-output-dir");
    REQUIRE_FALSE(std::filesystem::exists(out_dir));

    harness.configure({.output_dir = out_dir});

    REQUIRE(std::filesystem::is_directory(out_dir));
    std::error_code ec;
    std::filesystem::remove_all(out_dir, ec);
    REQUIRE_FALSE(ec);
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

TEST_CASE("ValidationHarness process_blocks zero blocks returns one silent buffer",
          "[harness][phase2][coverage][issue-646]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.buffer_size = 8, .output_channels = 2});

    auto output = harness.process_blocks(0);
    REQUIRE(output.size() == 16);
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

TEST_CASE("ValidationHarness process_buffer auto-prepares with caller channel layout",
          "[harness][phase2][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.buffer_size = 3, .input_channels = 1, .output_channels = 1});

    std::vector<float> input = {0.25f, -0.5f, 0.75f};
    auto output = harness.process_buffer(input, 1, 3);

    REQUIRE(output.size() == input.size());
    REQUIRE_THAT(static_cast<double>(output[0]), WithinAbs(0.25, 0.0001));
    REQUIRE_THAT(static_cast<double>(output[1]), WithinAbs(-0.5, 0.0001));
    REQUIRE_THAT(static_cast<double>(output[2]), WithinAbs(0.75, 0.0001));
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

TEST_CASE("ValidationHarness MIDI note-off and CC helpers reach process_buffer",
          "[harness][phase2]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.buffer_size = 4, .input_channels = 2, .output_channels = 2});
    harness.prepare();
    REQUIRE(last_processor != nullptr);

    harness.send_midi_note_on(0, 60, 100);
    harness.send_midi_note_off(0, 60);
    harness.send_midi_cc(0, 74, 64);

    std::vector<float> input(8, 0.0f);
    auto output = harness.process_buffer(input, 2, 4);
    REQUIRE(output.size() == 8);
    REQUIRE(last_processor->note_on_count_ == 1);
    REQUIRE(last_processor->note_off_count_ == 1);
    REQUIRE(last_processor->cc_count_ == 1);
}

TEST_CASE("ValidationHarness process_blocks consumes queued MIDI once",
          "[harness][phase2][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({.buffer_size = 4});
    harness.prepare();
    REQUIRE(last_processor != nullptr);

    harness.send_midi_note_on(0, 60, 100);
    auto output = harness.process_blocks(3);

    REQUIRE(output.size() == 8);
    REQUIRE(last_processor->note_on_count_ == 1);
    REQUIRE(last_processor->note_off_count_ == 0);
    REQUIRE(last_processor->cc_count_ == 0);
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

TEST_CASE("ValidationHarness report omits payload for unknown entry types",
          "[harness][phase2][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    pulp::format::ReportEntry entry;
    entry.type = "custom_probe";
    entry.status = pulp::format::ValidationStatus::pass;
    entry.target = "probe";
    entry.payload_json = "{\"should_not_appear\":true}";
    harness.add_entry(entry);

    const auto report = harness.generate_report();
    REQUIRE_THAT(report, ContainsSubstring("\"custom_probe\""));
    REQUIRE_THAT(report, ContainsSubstring("\"probe\""));
    REQUIRE(report.find("should_not_appear") == std::string::npos);
}

TEST_CASE("ValidationHarness report includes sanitizer payloads",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    pulp::format::ReportEntry entry;
    entry.type = "sanitizer";
    entry.status = pulp::format::ValidationStatus::fail;
    entry.target = "asan";
    entry.error_message = "heap-use-after-free";
    entry.payload_json = "{\"kind\":\"address\",\"reports\":1}";
    harness.add_entry(entry);

    const auto report = harness.generate_report();
    REQUIRE_THAT(report, ContainsSubstring("\"type\": \"sanitizer\""));
    REQUIRE_THAT(report, ContainsSubstring("\"status\": \"fail\""));
    REQUIRE_THAT(report, ContainsSubstring("\"error_message\": \"heap-use-after-free\""));
    REQUIRE_THAT(report, ContainsSubstring("\"sanitizer\": {\"kind\":\"address\",\"reports\":1}"));
}

TEST_CASE("ValidationHarness report escapes JSON metadata and entry strings",
          "[harness][phase2][coverage][issue-646]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({
        .git_ref = "branch/quote\"slash\\",
        .run_id = "run\nwith\ttabs",
    });

    pulp::format::ReportEntry entry;
    entry.type = "validator";
    entry.status = pulp::format::ValidationStatus::error;
    entry.target = "Plugin \"A\"";
    entry.error_message = "line1\nline2\\done";
    entry.payload_json = "{\"tool\":\"fake\"}";
    harness.add_entry(entry);

    auto report = harness.generate_report();
    REQUIRE_THAT(report, ContainsSubstring("branch/quote\\\"slash\\\\"));
    REQUIRE_THAT(report, ContainsSubstring("run\\nwith\\ttabs"));
    REQUIRE_THAT(report, ContainsSubstring("Plugin \\\"A\\\""));
    REQUIRE_THAT(report, ContainsSubstring("line1\\nline2\\\\done"));
}

TEST_CASE("ValidationHarness report escapes generic control characters",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    pulp::format::ReportEntry entry;
    entry.type = "test_suite";
    entry.status = pulp::format::ValidationStatus::pass;
    entry.target = std::string("suite") + static_cast<char>(0x01) + "name";
    entry.payload_json = "{\"total\":1}";
    harness.add_entry(entry);

    const auto report = harness.generate_report();
    REQUIRE_THAT(report, ContainsSubstring("suite\\u0001name"));
    REQUIRE_THAT(report, ContainsSubstring("\"test_suite\": {\"total\":1}"));
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

TEST_CASE("ValidationHarness write_report creates nested directories",
          "[harness][phase2][coverage][issue-646]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = make_temp_dir("pulp-harness-nested-report");
    auto report_path = dir / "a" / "b" / "report.json";
    REQUIRE(harness.write_report(report_path));
    REQUIRE(std::filesystem::is_regular_file(report_path));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("ValidationHarness write_report reports failure for directory paths",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = make_temp_dir("pulp-harness-report-as-dir");
    std::filesystem::create_directories(dir);
    REQUIRE_FALSE(harness.write_report(dir));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
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
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool\": \"nonexistent-validator-tool-xyz\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("fake-plugin-for-skip-test.vst3"));
    REQUIRE(harness.entries().size() == 1);

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

TEST_CASE("ValidationHarness run_validator errors on unknown installed tool",
          "[harness][phase2][coverage][issue-646]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto tmp_plugin = make_temp_path("fake-plugin-for-unknown-validator");
    { std::ofstream f(tmp_plugin); f << "dummy"; }

    auto entry = harness.run_validator("sh", tmp_plugin);
    REQUIRE(entry.status == pulp::format::ValidationStatus::error);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("Unknown validator tool: sh"));

    std::error_code ec;
    std::filesystem::remove(tmp_plugin, ec);
    REQUIRE_FALSE(ec);
}

#if !defined(_WIN32)
TEST_CASE("ValidationHarness disables plugin editors for external validators",
          "[harness][issue-2515]") {
    auto dir = make_temp_dir("pulp-harness-validator-env");
    std::filesystem::create_directories(dir);
    auto plugin = dir / "Fake.vst3";
    auto tool = dir / "pluginval";
    auto env_out = dir / "env.txt";

    {
        std::ofstream f(plugin);
        f << "dummy";
    }
    {
        std::ofstream f(tool);
        f << "#!/bin/sh\n"
             "if [ \"$1\" = \"--version\" ]; then echo fake-pluginval; exit 0; fi\n"
             "printf '%s:%s:%s\\n' \"$PULP_DISABLE_PLUGIN_EDITOR\" "
             "\"$PULP_HEADLESS\" \"$PULP_TEST_MODE\" > \"$PULP_VALIDATOR_ENV_OUT\"\n"
             "exit 0\n";
    }
    std::filesystem::permissions(
        tool,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add);

    ScopedEnv path("PATH");
    ScopedEnv output("PULP_VALIDATOR_ENV_OUT");
    const char* existing_path = std::getenv("PATH");
    path.set(dir.string()
             + (existing_path ? ":" + std::string(existing_path) : std::string{}));
    output.set(env_out.string());

    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});
    auto entry = harness.run_validator("pluginval", plugin);

    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);

    std::ifstream f(env_out);
    std::string captured;
    std::getline(f, captured);
    REQUIRE(captured == "1:1:1");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("ValidationHarness run_validator captures installed pluginval success",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = make_temp_dir("pulp-harness-pluginval-pass");
    std::filesystem::create_directories(dir);
    auto plugin = dir / "Fake Plugin.vst3";
    { std::ofstream f(plugin); f << "dummy"; }

    write_executable_script(
        dir,
        "pluginval",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then\n"
        "  printf 'pluginval fake 1.2.3\\n'\n"
        "  exit 0\n"
        "fi\n"
        "printf 'validated:%s\\n' \"$*\"\n"
        "exit 0\n");

    ScopedPathPrefix path_prefix(dir);
    auto entry = harness.run_validator("pluginval", plugin);

    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE(entry.error_message.empty());
    REQUIRE(entry.target == plugin.filename().string());
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool\": \"pluginval\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool_version\": \"pluginval fake 1.2.3\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"plugin_format\": \"vst3\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"exit_code\": 0"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("--strictness-level 5"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("--timeout-ms 30000"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("Fake Plugin.vst3"));
    REQUIRE(harness.entries().size() == 1);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("ValidationHarness run_validator records installed pluginval failures",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = make_temp_dir("pulp-harness-pluginval-fail");
    std::filesystem::create_directories(dir);
    auto plugin = dir / "Broken.vst3";
    { std::ofstream f(plugin); f << "dummy"; }

    write_executable_script(
        dir,
        "pluginval",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then\n"
        "  printf 'pluginval fake 9.9.9\\n'\n"
        "  exit 0\n"
        "fi\n"
        "printf 'validator stdout\\n'\n"
        "printf 'validator stderr\\n' >&2\n"
        "exit 7\n");

    ScopedPathPrefix path_prefix(dir);
    auto entry = harness.run_validator("pluginval", plugin);

    REQUIRE(entry.status == pulp::format::ValidationStatus::fail);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("pluginval exited with code 7"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool_version\": \"pluginval fake 9.9.9\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"plugin_format\": \"vst3\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"exit_code\": 7"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("validator stdout"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("validator stderr"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("ValidationHarness run_validator defaults clap-validator format",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = make_temp_dir("pulp-harness-clap-validator");
    std::filesystem::create_directories(dir);
    auto plugin = dir / "Example.clap";
    { std::ofstream f(plugin); f << "dummy"; }

    write_executable_script(
        dir,
        "clap-validator",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then\n"
        "  printf 'clap-validator fake 0.4\\n'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" != \"validate\" ]; then\n"
        "  exit 11\n"
        "fi\n"
        "printf 'validated clap:%s\\n' \"$2\"\n"
        "exit 0\n");

    ScopedPathPrefix path_prefix(dir);
    auto entry = harness.run_validator("clap-validator", plugin);

    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE(entry.error_message.empty());
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool\": \"clap-validator\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool_version\": \"clap-validator fake 0.4\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"plugin_format\": \"clap\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"exit_code\": 0"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("validated clap:"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("ValidationHarness run_validator defaults auval format",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = make_temp_dir("pulp-harness-auval-pass");
    std::filesystem::create_directories(dir);
    auto plugin = dir / "Fake.component";
    { std::ofstream f(plugin); f << "dummy"; }

    write_executable_script(
        dir,
        "auval",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then\n"
        "  printf 'auval fake 2.0\\n'\n"
        "  exit 0\n"
        "fi\n"
        "printf 'auval args:%s\\n' \"$*\"\n"
        "exit 0\n");

    ScopedPathPrefix path_prefix(dir);
    auto entry = harness.run_validator("auval", plugin);

    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE(entry.error_message.empty());
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool\": \"auval\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool_version\": \"auval fake 2.0\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"plugin_format\": \"au\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("-v aufx pulp Pulp"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
}

TEST_CASE("ValidationHarness run_validator defaults vstvalidator format and trims CRLF versions",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto dir = make_temp_dir("pulp-harness-vstvalidator-pass");
    std::filesystem::create_directories(dir);
    auto plugin = dir / "Fake.vst3";
    { std::ofstream f(plugin); f << "dummy"; }

    write_executable_script(
        dir,
        "vstvalidator",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then\n"
        "  printf 'vstvalidator fake 3.0\\r\\n'\n"
        "  exit 0\n"
        "fi\n"
        "printf 'vstvalidator saw:%s\\n' \"$1\"\n"
        "exit 0\n");

    ScopedPathPrefix path_prefix(dir);
    auto entry = harness.run_validator("vstvalidator", plugin);

    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE(entry.error_message.empty());
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool\": \"vstvalidator\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"tool_version\": \"vstvalidator fake 3.0\""));
    REQUIRE(entry.payload_json.find("fake 3.0\\r") == std::string::npos);
    REQUIRE(entry.payload_json.find("fake 3.0\\n") == std::string::npos);
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"plugin_format\": \"vst3\""));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("vstvalidator saw:"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
}
#endif

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

TEST_CASE("ValidationHarness clearing screenshot provider restores skip behavior",
          "[harness][phase2][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.set_capture_screenshot_provider(
        [](const std::filesystem::path&, uint32_t, uint32_t, float, const std::string&) {
            return true;
        });
    REQUIRE(harness.capture_screenshot("/tmp/test-provider-clear.png").status ==
            pulp::format::ValidationStatus::pass);

    harness.set_capture_screenshot_provider({});
    auto entry = harness.capture_screenshot("/tmp/test-provider-clear.png");
    REQUIRE(entry.status == pulp::format::ValidationStatus::skip);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("provider"));
}

TEST_CASE("ValidationHarness screenshot passes configured capture options to provider",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({
        .screenshot_width = 320,
        .screenshot_height = 180,
        .screenshot_scale = 1.5f,
        .screenshot_backend = "software\"test\\gpu",
    });

    float captured_scale = 0.0f;
    std::string captured_backend;
    harness.set_capture_screenshot_provider(
        [&](const std::filesystem::path&, uint32_t w, uint32_t h,
            float scale, const std::string& backend) {
            REQUIRE(w == 320);
            REQUIRE(h == 180);
            captured_scale = scale;
            captured_backend = backend;
            return true;
        });

    auto entry = harness.capture_screenshot("/tmp/test-options.png");
    REQUIRE(entry.status == pulp::format::ValidationStatus::pass);
    REQUIRE_THAT(static_cast<double>(captured_scale), WithinAbs(1.5, 0.001));
    REQUIRE(captured_backend == "software\"test\\gpu");
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"width\": 320"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"backend\": \"software\\\"test\\\\gpu\""));
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

TEST_CASE("ValidationHarness clearing inspector provider restores skip behavior",
          "[harness][phase2][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.set_capture_inspector_provider(
        []() { return std::string("{\"type\":\"View\"}"); });
    REQUIRE(harness.capture_inspector().status == pulp::format::ValidationStatus::pass);

    harness.set_capture_inspector_provider({});
    auto entry = harness.capture_inspector();
    REQUIRE(entry.status == pulp::format::ValidationStatus::skip);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("provider"));
}

TEST_CASE("ValidationHarness inspector fails when provider returns empty tree",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    harness.set_capture_inspector_provider([]() { return std::string{}; });

    auto entry = harness.capture_inspector();
    REQUIRE(entry.status == pulp::format::ValidationStatus::fail);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("empty tree"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"view_count\": 0"));
}

TEST_CASE("ValidationHarness compare_screenshots missing inputs -> error",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto existing = make_temp_path("pulp-harness-existing");
    std::ofstream(existing) << "content";
    auto missing_ref = make_temp_path("pulp-harness-missing-ref");
    auto missing_rendered = make_temp_path("pulp-harness-missing-rendered");

    auto entry = harness.compare_screenshots(missing_ref, existing);
    REQUIRE(entry.status == pulp::format::ValidationStatus::error);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("Reference file not found"));

    entry = harness.compare_screenshots(existing, missing_rendered);
    REQUIRE(entry.status == pulp::format::ValidationStatus::error);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("Rendered file not found"));

    std::filesystem::remove(existing);
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

TEST_CASE("ValidationHarness compare_screenshots different sizes -> fail",
          "[harness][phase2][issue-298]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({});

    auto a = std::filesystem::temp_directory_path() / "harness-ref-size.bin";
    auto b = std::filesystem::temp_directory_path() / "harness-ren-size.bin";
    std::ofstream(a) << "short";
    std::ofstream(b) << "longer-content";

    auto entry = harness.compare_screenshots(a, b);
    REQUIRE(entry.status == pulp::format::ValidationStatus::fail);
    REQUIRE_THAT(entry.error_message, ContainsSubstring("differ"));
    REQUIRE_THAT(entry.payload_json, ContainsSubstring("\"similarity\": 0"));
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

TEST_CASE("ValidationHarness report renders capture and diff payload sections",
          "[harness][coverage][phase3]") {
    pulp::format::ValidationHarness harness(create_test_gain);
    harness.configure({
        .diff_tolerance = 7,
        .diff_threshold = 0.42f,
    });

    auto dir = make_temp_dir("pulp-harness-report-payloads");
    std::filesystem::create_directories(dir);
    auto screenshot = dir / "shot.png";
    auto ref = dir / "ref.bin";
    auto rendered = dir / "rendered.bin";
    { std::ofstream f(ref); f << "A"; }
    { std::ofstream f(rendered); f << "B"; }

    harness.set_capture_screenshot_provider(
        [](const std::filesystem::path& p, uint32_t, uint32_t, float, const std::string&) {
            std::ofstream(p) << "PNG";
            return true;
        });
    harness.set_capture_inspector_provider(
        []() { return std::string("{\"type\":\"Root\",\"children\":[]}"); });

    REQUIRE(harness.capture_screenshot(screenshot).status ==
            pulp::format::ValidationStatus::pass);
    REQUIRE(harness.compare_screenshots(ref, rendered).status ==
            pulp::format::ValidationStatus::fail);
    REQUIRE(harness.capture_inspector().status ==
            pulp::format::ValidationStatus::pass);

    const auto report = harness.generate_report();
    REQUIRE_THAT(report, ContainsSubstring("\"screenshot\": {"));
    REQUIRE_THAT(report, ContainsSubstring("\"diff\": {"));
    REQUIRE_THAT(report, ContainsSubstring("\"inspector\": {"));
    REQUIRE_THAT(report, ContainsSubstring("\"tolerance\": 7"));
    REQUIRE_THAT(report, ContainsSubstring("\"threshold\": 0.42"));
    REQUIRE_THAT(report, ContainsSubstring("\"comparison\": \"byte-level\""));
    REQUIRE_THAT(report, ContainsSubstring("\"view_tree\": {\"type\":\"Root\""));
    REQUIRE_THAT(report, ContainsSubstring("    },\n    {"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    REQUIRE_FALSE(ec);
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

TEST_CASE("ValidationStatus to_string falls back to error for unknown values",
          "[harness][coverage][phase3]") {
    using pulp::format::ValidationStatus;
    using pulp::format::to_string;

    REQUIRE(std::string(to_string(static_cast<ValidationStatus>(99))) == "error");
}
