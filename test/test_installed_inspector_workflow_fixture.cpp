#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef PULP_INSTALLED_INSPECTOR_WORKFLOW_FIXTURE
#error "PULP_INSTALLED_INSPECTOR_WORKFLOW_FIXTURE must name the real workflow fixture"
#endif

namespace {

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) previous_ = value;
    }
    ~ScopedEnv() {
        if (previous_)
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        else
            ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

struct ScratchDir {
    std::filesystem::path path;
    ScratchDir() {
        path = std::filesystem::temp_directory_path()
            / ("pulp-installed-inspector-workflow-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

} // namespace

TEST_CASE("Installed workflow fixture exposes real scripted UI, state, and compositor capture",
          "[standalone][inspect][installed-workflow][gpu]") {
    ScratchDir scratch;
    ScopedEnv runtime_env("PULP_INSPECTOR_RUNTIME_DIR");
    ScopedEnv profile_env("PULP_INSPECT_PROFILE");
    ScopedEnv headless_env("PULP_HEADLESS");
    ScopedEnv test_mode_env("PULP_TEST_MODE");
    ScopedEnv ci_env("CI");
    const auto runtime_path = scratch.path / "runtime";
    ::setenv("PULP_INSPECTOR_RUNTIME_DIR", runtime_path.string().c_str(), 1);
    ::unsetenv("PULP_INSPECT_PROFILE");
    ::unsetenv("PULP_HEADLESS");
    ::unsetenv("PULP_TEST_MODE");
    ::unsetenv("CI");

    const auto ready_path = scratch.path / "ready.json";
    const auto stop_path = scratch.path / "stop";
    std::mutex startup_log_mutex;
    std::string startup_log;
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 45'000;
    options.on_stderr_line = [&](std::string_view line) {
        std::lock_guard lock(startup_log_mutex);
        startup_log.append(line);
        startup_log.push_back('\n');
    };
    pulp::platform::ChildProcess child;
    REQUIRE(child.start(PULP_INSTALLED_INSPECTOR_WORKFLOW_FIXTURE,
                        {"--ready", ready_path.string(),
                         "--stop", stop_path.string()}, options));

    pulp::inspect::InspectorDiscoveryReader reader(runtime_path);
    std::vector<pulp::inspect::InspectorDiscoveryRecord> records;
    const auto ready_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((!std::filesystem::exists(ready_path) || records.size() != 1)
           && child.is_running()
           && std::chrono::steady_clock::now() < ready_deadline) {
        records = reader.list();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::string startup_stdout;
    std::string startup_stderr;
    const auto startup_process_id = child.process_id();
    const bool startup_process_running = child.is_running();
    if (records.size() != 1 || !std::filesystem::exists(ready_path)) {
        child.cancel();
        const auto result = child.wait();
        startup_stdout = result.stdout_output;
        startup_stderr = result.stderr_output;
    }
    INFO("startup stdout=" << startup_stdout);
    INFO("startup stderr=" << startup_stderr);
    {
        std::lock_guard lock(startup_log_mutex);
        INFO("streamed startup stderr=" << startup_log);
    }
    INFO("startup pid=" << startup_process_id);
    INFO("startup process running before cancellation=" << startup_process_running);
    REQUIRE(std::filesystem::exists(ready_path));
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().plugin_id
            == "com.pulp.test.installed-inspector-workflow");
    std::ifstream ready_input(ready_path);
    const auto ready_json = choc::json::parse(
        std::string(std::istreambuf_iterator<char>(ready_input), {}));
    REQUIRE(ready_json["session_id"].getString() == records.front().session_id);
    REQUIRE(ready_json["instance_id"].getString() == records.front().instance_id);
    REQUIRE(ready_json["publication_id"].getString()
            == records.front().publication_id);
    REQUIRE(ready_json["runtime_dir"].getString()
            == runtime_path.generic_string());

    pulp::inspect::InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));

    const auto context = client.request(
        "Inspector.getAgentContext", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(context.is_error);
    REQUIRE(context.params_json.find(records.front().session_id)
            != std::string::npos);

    const auto document = client.request(
        "DOM.getDocument", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(document.is_error);
    REQUIRE(document.params_json.find("REAL INSTALLED INSPECTOR WORKFLOW")
            != std::string::npos);
    REQUIRE(document.params_json.find("workflow-gain") != std::string::npos);

    const auto parameters = client.request(
        "State.getParameters", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(parameters.is_error);
    const auto parameter_array = choc::json::parse(parameters.params_json);
    REQUIRE(parameter_array.isArray());
    REQUIRE(parameter_array.size() > 0);

    std::int64_t parameter_id = -1;
    double minimum = 0.0;
    double maximum = 0.0;
    double original = 0.0;
    for (std::size_t i = 0; i < parameter_array.size(); ++i) {
        const auto parameter = parameter_array[static_cast<std::uint32_t>(i)];
        if (parameter["name"].getString() == "Workflow Gain") {
            parameter_id = parameter["id"].getInt64();
            minimum = parameter["min"].getWithDefault<double>(0.0);
            maximum = parameter["max"].getWithDefault<double>(0.0);
            original = parameter["value"].getWithDefault<double>(0.0);
            break;
        }
    }
    REQUIRE(parameter_id >= 0);
    REQUIRE(maximum > minimum);
    const double target = minimum + ((maximum - minimum) * 0.73);
    REQUIRE(target != Catch::Approx(original));

    REQUIRE_FALSE(client.request(
        "Session.acquireController", "{}", std::chrono::seconds(2)).is_error);
    const auto set_json = std::string("{\"id\":")
        + std::to_string(parameter_id) + ",\"value\":"
        + std::to_string(target) + "}";
    REQUIRE_FALSE(client.request(
        "State.setParameter", set_json, std::chrono::seconds(2)).is_error);

    const auto updated = client.request(
        "State.getParameters", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(updated.is_error);
    const auto updated_array = choc::json::parse(updated.params_json);
    bool observed_target = false;
    for (std::size_t i = 0; i < updated_array.size(); ++i) {
        const auto parameter = updated_array[static_cast<std::uint32_t>(i)];
        if (parameter["id"].getInt64() == parameter_id) {
            observed_target = parameter["value"].getWithDefault<double>(minimum)
                == Catch::Approx(target).margin(0.001);
        }
    }
    REQUIRE(observed_target);

    std::vector<std::uint8_t> png;
    std::int64_t width = 0;
    std::int64_t height = 0;
    const auto capture_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (png.empty() && std::chrono::steady_clock::now() < capture_deadline) {
        const auto capture = client.request(
            "Capture.screenshot", "{}", std::chrono::seconds(2));
        if (!capture.is_error) {
            const auto capture_json = choc::json::parse(capture.params_json);
            width = capture_json["width"].getWithDefault<std::int64_t>(0);
            height = capture_json["height"].getWithDefault<std::int64_t>(0);
            if (auto decoded = pulp::runtime::base64_decode(
                    capture_json["data"].getString())) {
                if (pulp::view::analyze_screenshot_content(*decoded)
                        .passes_content_floor())
                    png = std::move(*decoded);
            }
        }
        if (png.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(width > 1);
    REQUIRE(height > 1);
    REQUIRE(png.size() > 32);
    const std::array<std::uint8_t, 8> png_signature{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    REQUIRE(std::equal(png_signature.begin(), png_signature.end(), png.begin()));
    const auto screenshot_hash =
        pulp::runtime::sha256_hex(png.data(), png.size());
    INFO("real compositor screenshot sha256=" << screenshot_hash);
    REQUIRE(screenshot_hash.size() == 64);
    REQUIRE(screenshot_hash != std::string(64, '0'));

    client.disconnect();
    {
        std::ofstream stop(stop_path);
        stop << "stop\n";
    }
    const auto result = child.wait();
    INFO("stdout=" << result.stdout_output);
    INFO("stderr=" << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE(reader.list().empty());
}
