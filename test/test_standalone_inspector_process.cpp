#include <catch2/catch_test_macros.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef PULP_STANDALONE_INSPECTOR_PROCESS_FIXTURE
#error "PULP_STANDALONE_INSPECTOR_PROCESS_FIXTURE must name the real standalone fixture"
#endif

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) previous_ = value;
    }

    ~ScopedEnv() {
        if (previous_)
            set(*previous_);
        else
            unset();
    }

    void set(const std::string& value) {
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    void unset() {
        ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

struct ScratchDir {
    std::filesystem::path path;

    explicit ScratchDir(const char* stem) {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path()
            / (std::string(stem) + "-" + suffix);
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path);
    }

    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

void configure_real_window_environment(const ScratchDir& scratch,
                                       std::string_view profile) {
    ::setenv("PULP_INSPECTOR_RUNTIME_DIR",
             (scratch.path / "runtime").string().c_str(), 1);
    ::setenv("PULP_INSPECT_PROFILE", std::string(profile).c_str(), 1);
    ::unsetenv("PULP_HEADLESS");
    ::unsetenv("PULP_TEST_MODE");
    ::unsetenv("CI");
}

} // namespace

TEST_CASE("Explicit standalone subprocess serves its selected compositor frame",
          "[standalone][inspect][process][gpu]") {
    ScratchDir scratch("pulp-inspector-process-enabled");
    ScopedEnv runtime_dir("PULP_INSPECTOR_RUNTIME_DIR");
    ScopedEnv profile("PULP_INSPECT_PROFILE");
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
    configure_real_window_environment(scratch, "develop");

    const auto exit_png = scratch.path / "exit.png";
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 15'000;
    pulp::platform::ChildProcess child;
    REQUIRE(child.start(
        PULP_STANDALONE_INSPECTOR_PROCESS_FIXTURE,
        {"--exit-screenshot", exit_png.string(), "--frames", "180"}, options));

    const auto runtime_path = scratch.path / "runtime";
    pulp::inspect::InspectorDiscoveryReader reader(runtime_path);
    std::vector<pulp::inspect::InspectorDiscoveryRecord> records;
    const auto discovery_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (records.empty() && child.is_running()
           && std::chrono::steady_clock::now() < discovery_deadline) {
        records = reader.list();
        if (records.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (records.empty()) {
        child.cancel();
        const auto result = child.wait();
        INFO("stdout=" << result.stdout_output);
        INFO("stderr=" << result.stderr_output);
    }
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().plugin_id
            == "com.pulp.test.inspector-process-fixture");

    pulp::inspect::InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    std::vector<std::uint8_t> captured_png;
    const auto capture_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (captured_png.empty()
           && std::chrono::steady_clock::now() < capture_deadline) {
        const auto response = client.request(
            "Capture.screenshot", "{}", std::chrono::seconds(1));
        if (!response.is_error) {
            const auto json = choc::json::parse(response.params_json);
            auto decoded = pulp::runtime::base64_decode(json["data"].getString());
            if (decoded) {
                const auto stats = pulp::view::analyze_screenshot_content(*decoded);
                if (stats.passes_content_floor()) captured_png = std::move(*decoded);
            }
        }
        if (captured_png.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE_FALSE(captured_png.empty());
    const auto inspector_png = scratch.path / "inspector.png";
    {
        std::ofstream output(inspector_png, std::ios::binary);
        output.write(reinterpret_cast<const char*>(captured_png.data()),
                     static_cast<std::streamsize>(captured_png.size()));
    }
    REQUIRE(std::filesystem::file_size(inspector_png) == captured_png.size());
    client.disconnect();

    const auto result = child.wait();
    INFO("stdout=" << result.stdout_output);
    INFO("stderr=" << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    const auto exit_capture = read_bytes(exit_png);
    REQUIRE_FALSE(exit_capture.empty());
    REQUIRE(pulp::view::analyze_screenshot_content(exit_capture)
                .passes_content_floor());
    REQUIRE(reader.list().empty());
}

TEST_CASE("Off-mode standalone subprocess creates no inspector artifact",
          "[standalone][inspect][process][negative][gpu]") {
    ScratchDir scratch("pulp-inspector-process-off");
    ScopedEnv runtime_dir("PULP_INSPECTOR_RUNTIME_DIR");
    ScopedEnv profile("PULP_INSPECT_PROFILE");
    ScopedEnv headless("PULP_HEADLESS");
    ScopedEnv test_mode("PULP_TEST_MODE");
    ScopedEnv ci("CI");
    configure_real_window_environment(scratch, "off");

    const auto exit_png = scratch.path / "off-exit.png";
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 10'000;
    pulp::platform::ChildProcess child;
    REQUIRE(child.start(
        PULP_STANDALONE_INSPECTOR_PROCESS_FIXTURE,
        {"--exit-screenshot", exit_png.string(), "--frames", "60"}, options));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / "runtime"));

    const auto result = child.wait();
    INFO("stderr=" << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(std::filesystem::exists(exit_png));
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / "runtime"));
}

TEST_CASE("Ordinary DSP-only subprocess ignores ambient inspector activation",
          "[standalone][inspect][process][headless][negative]") {
    ScratchDir scratch("pulp-inspector-process-headless");
    ScopedEnv runtime_dir("PULP_INSPECTOR_RUNTIME_DIR");
    ScopedEnv profile("PULP_INSPECT_PROFILE");
    runtime_dir.set((scratch.path / "runtime").string());
    profile.set("develop");

    pulp::platform::ProcessOptions options;
    options.timeout_ms = 5'000;
    const auto result = pulp::platform::ChildProcess::run(
        PULP_STANDALONE_INSPECTOR_PROCESS_FIXTURE, {"--dsp-only"}, options);
    INFO("stderr=" << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(std::filesystem::exists(scratch.path / "runtime"));
}
