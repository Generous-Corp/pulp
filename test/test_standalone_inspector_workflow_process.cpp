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
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef PULP_STANDALONE_INSPECTOR_WORKFLOW_PROCESS_FIXTURE
#error "PULP_STANDALONE_INSPECTOR_WORKFLOW_PROCESS_FIXTURE must name the real workflow fixture"
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
            / ("pulp-standalone-inspector-workflow-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void wait_for_external_state_change_poll() {
    static std::mutex mutex;
    static std::condition_variable cv;
    std::unique_lock lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(10));
}

std::optional<double> dom_value_for_id(choc::value::ValueView node,
                                       std::string_view id) {
    if (!node.isObject()) return std::nullopt;
    if (node.hasObjectMember("id") && node["id"].isString()
        && node["id"].getString() == id && node.hasObjectMember("value")) {
        return node["value"].getWithDefault<double>(-1.0);
    }
    if (!node.hasObjectMember("children") || !node["children"].isArray())
        return std::nullopt;
    const auto children = node["children"];
    for (std::uint32_t i = 0; i < children.size(); ++i) {
        if (auto value = dom_value_for_id(children[i], id)) return value;
    }
    return std::nullopt;
}

bool runtime_contains_only_inert_locks(
    const std::filesystem::path& runtime_path,
    const std::vector<std::filesystem::path>& ownership_paths) {
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(runtime_path, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (std::find(ownership_paths.begin(), ownership_paths.end(), iterator->path())
                == ownership_paths.end()
            || !iterator->is_regular_file(error) || error) {
            return false;
        }
    }
    return !error;
}

pulp::inspect::InspectorMessage controlled_request(
    pulp::inspect::InspectorClient& client, std::string method,
    std::string params_json = "{}") {
    const auto timeout = std::chrono::seconds(2);
    const auto lease = client.request("Session.acquireController", "{}", timeout);
    if (lease.is_error) return lease;
    auto response = client.request(std::move(method), std::move(params_json), timeout);
    const auto release = client.request("Session.releaseController", "{}", timeout);
    if (!response.is_error && release.is_error) return release;
    return response;
}

} // namespace

TEST_CASE("Standalone source-build workflow exposes real scripted UI, state, and compositor capture",
          "[standalone][inspect][process][workflow][gpu]") {
    ScratchDir scratch;
    ScopedEnv runtime_env("PULP_INSPECTOR_RUNTIME_DIR");
    ScopedEnv profile_env("PULP_INSPECT_PROFILE");
    ScopedEnv runtime_eval_env("PULP_INSPECT_RUNTIME_EVAL");
    ScopedEnv headless_env("PULP_HEADLESS");
    ScopedEnv test_mode_env("PULP_TEST_MODE");
    ScopedEnv ci_env("CI");
    const auto runtime_path = scratch.path / "runtime";
    ::unsetenv("PULP_INSPECTOR_RUNTIME_DIR");
    ::unsetenv("PULP_INSPECT_PROFILE");
    ::unsetenv("PULP_INSPECT_RUNTIME_EVAL");
    ::unsetenv("PULP_HEADLESS");
    ::unsetenv("PULP_TEST_MODE");
    ::unsetenv("CI");

    const auto develop_path = scratch.path / "develop";
    const auto observe_path = scratch.path / "observe";
    const auto eval_path = scratch.path / "eval";
    const auto ready_path = develop_path / "ready.json";
    const auto stop_path = develop_path / "stop";
    const auto observation_path = develop_path / "test-input-observation.json";
    const auto reload_path = develop_path / "reload.request";
    const auto observe_ready_path = observe_path / "ready.json";
    const auto observe_stop_path = observe_path / "stop";
    const auto eval_ready_path = eval_path / "ready.json";
    const auto eval_stop_path = eval_path / "stop";
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 45'000;
    // These long-lived children are polled before wait(), so captured GPU logs
    // would not be drained and can fill a pipe before readiness is published.
    options.capture_stdout = false;
    options.capture_stderr = false;
    pulp::platform::ChildProcess child;
    REQUIRE(child.start(PULP_STANDALONE_INSPECTOR_WORKFLOW_PROCESS_FIXTURE,
                        {"--ready", ready_path.string(),
                         "--stop", stop_path.string(),
                         "--runtime-dir", runtime_path.string(),
                         "--observation", observation_path.string(),
                         "--reload", reload_path.string(),
                         "--runtime-eval",
                         "--wait-until-stop"}, options));
    pulp::platform::ChildProcess observe_child;
    REQUIRE(observe_child.start(PULP_STANDALONE_INSPECTOR_WORKFLOW_PROCESS_FIXTURE,
                        {"--ready", observe_ready_path.string(),
                         "--stop", observe_stop_path.string(),
                         "--runtime-dir", runtime_path.string(),
                         "--profile", "observe",
                         "--wait-until-stop"}, options));
    pulp::platform::ChildProcess eval_child;
    REQUIRE(eval_child.start(PULP_STANDALONE_INSPECTOR_WORKFLOW_PROCESS_FIXTURE,
                        {"--ready", eval_ready_path.string(),
                         "--stop", eval_stop_path.string(),
                         "--runtime-dir", runtime_path.string(),
                         "--profile", "custom",
                         "--capability", "session.describe",
                         "--capability", "session.control",
                         "--capability", "runtime.eval",
                         "--runtime-eval",
                         "--no-value-channels",
                         "--no-reload",
                         "--wait-until-stop"}, options));

    pulp::inspect::InspectorDiscoveryReader reader(runtime_path);
    std::vector<pulp::inspect::InspectorDiscoveryRecord> records;
    const auto ready_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((!std::filesystem::exists(ready_path)
            || !std::filesystem::exists(observe_ready_path)
            || !std::filesystem::exists(eval_ready_path) || records.size() != 3)
           && child.is_running() && observe_child.is_running() && eval_child.is_running()
           && std::chrono::steady_clock::now() < ready_deadline) {
        records = reader.list();
        wait_for_external_state_change_poll();
    }
    std::string startup_stdout;
    std::string startup_stderr;
    std::string observe_startup_stderr;
    std::string eval_startup_stderr;
    const auto startup_process_id = child.process_id();
    const bool startup_process_running = child.is_running();
    if (records.size() != 3 || !std::filesystem::exists(ready_path)
        || !std::filesystem::exists(observe_ready_path)
        || !std::filesystem::exists(eval_ready_path)) {
        child.cancel();
        observe_child.cancel();
        eval_child.cancel();
        const auto result = child.wait();
        const auto observe_result = observe_child.wait();
        const auto eval_result = eval_child.wait();
        startup_stdout = result.stdout_output;
        startup_stderr = result.stderr_output;
        observe_startup_stderr = observe_result.stderr_output;
        eval_startup_stderr = eval_result.stderr_output;
    }
    INFO("startup stdout=" << startup_stdout);
    INFO("startup stderr=" << startup_stderr);
    INFO("observe startup stderr=" << observe_startup_stderr);
    INFO("eval startup stderr=" << eval_startup_stderr);
    INFO("startup pid=" << startup_process_id);
    INFO("startup process running before cancellation=" << startup_process_running);
    REQUIRE(std::filesystem::exists(ready_path));
    REQUIRE(std::filesystem::exists(observe_ready_path));
    REQUIRE(std::filesystem::exists(eval_ready_path));
    REQUIRE(records.size() == 3);
    std::ifstream ready_input(ready_path);
    const auto ready_json = choc::json::parse(
        std::string(std::istreambuf_iterator<char>(ready_input), {}));
    std::ifstream observe_ready_input(observe_ready_path);
    const auto observe_ready_json = choc::json::parse(
        std::string(std::istreambuf_iterator<char>(observe_ready_input), {}));
    std::ifstream eval_ready_input(eval_ready_path);
    const auto eval_ready_json = choc::json::parse(
        std::string(std::istreambuf_iterator<char>(eval_ready_input), {}));
    const auto find_record = [&](const auto& ready) -> const pulp::inspect::InspectorDiscoveryRecord& {
        const auto session_id = ready["session_id"].getString();
        const auto match = std::find_if(records.begin(), records.end(), [&](const auto& record) {
            return record.session_id == session_id;
        });
        REQUIRE(match != records.end());
        return *match;
    };
    const auto& develop_record = find_record(ready_json);
    const auto& observe_record = find_record(observe_ready_json);
    const auto& eval_record = find_record(eval_ready_json);
    REQUIRE(develop_record.session_id != observe_record.session_id);
    REQUIRE(develop_record.instance_id != observe_record.instance_id);
    REQUIRE(develop_record.publication_id != observe_record.publication_id);
    REQUIRE(eval_record.session_id != develop_record.session_id);
    REQUIRE(eval_record.session_id != observe_record.session_id);
    REQUIRE(develop_record.plugin_id == "com.pulp.test.standalone-inspector-workflow");
    REQUIRE(observe_record.plugin_id == develop_record.plugin_id);
    REQUIRE(ready_json["instance_id"].getString() == develop_record.instance_id);
    REQUIRE(ready_json["publication_id"].getString() == develop_record.publication_id);
    REQUIRE(ready_json["runtime_dir"].getString()
            == runtime_path.generic_string());

    std::string ambiguous_error;
    REQUIRE_FALSE(pulp::inspect::select_inspector_session(
        records, {}, {}, {}, &ambiguous_error));
    REQUIRE(ambiguous_error
            == "Multiple inspector sessions are live; specify a session ID");

    pulp::inspect::InspectorClient client;
    REQUIRE(client.connect(develop_record, reader));

    pulp::inspect::InspectorClient observe_client;
    REQUIRE(observe_client.connect(observe_record, reader));
    const auto observe_document = observe_client.request(
        "DOM.getDocument", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(observe_document.is_error);
    const auto observe_denial = observe_client.request(
        "State.setParameter", R"({"id":42017,"value":0.25})",
        std::chrono::seconds(2));
    REQUIRE(observe_denial.is_error);
    REQUIRE(observe_denial.error_code == "capability_denied");
    const auto observe_eval_denial = observe_client.request(
        "Runtime.evaluate", R"({"code":"1 + 1"})", std::chrono::seconds(2));
    REQUIRE(observe_eval_denial.is_error);
    REQUIRE(observe_eval_denial.error_code == "capability_unavailable");

    pulp::inspect::InspectorClient eval_client;
    REQUIRE(eval_client.connect(eval_record, reader));
    const auto eval_capabilities = eval_client.request(
        "Session.getCapabilities", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(eval_capabilities.is_error);
    const auto eval_capabilities_json =
        choc::json::parse(eval_capabilities.params_json);
    const auto eval_effective = eval_capabilities_json["effective"];
    REQUIRE(eval_effective.size() == 3);
    bool eval_has_describe = false;
    bool eval_has_control = false;
    bool eval_has_runtime = false;
    for (std::uint32_t i = 0; i < eval_effective.size(); ++i) {
        const auto id = eval_effective[i].getString();
        eval_has_describe |= id == "session.describe";
        eval_has_control |= id == "session.control";
        eval_has_runtime |= id == "runtime.eval";
    }
    REQUIRE(eval_has_describe);
    REQUIRE(eval_has_control);
    REQUIRE(eval_has_runtime);
    const auto eval_controller = eval_client.request(
        "Session.acquireController", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(eval_controller.is_error);
    const auto evaluated = eval_client.request(
        "Runtime.evaluate", R"({"code":"6 * 7"})", std::chrono::seconds(2));
    REQUIRE_FALSE(evaluated.is_error);
    REQUIRE(choc::json::parse(evaluated.params_json)
                ["result"].getWithDefault<std::int64_t>(0) == 42);
    const std::string oversized_code(65'537, 'a');
    const auto oversized_eval = eval_client.request(
        "Runtime.evaluate", "{\"code\":\"" + oversized_code + "\"}",
        std::chrono::seconds(2));
    REQUIRE(oversized_eval.is_error);
    REQUIRE(oversized_eval.params_json.find("65536-byte limit")
            != std::string::npos);

    const auto context = client.request(
        "Inspector.getAgentContext", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(context.is_error);
    REQUIRE(context.params_json.find(develop_record.session_id)
            != std::string::npos);

    const auto document = client.request(
        "DOM.getDocument", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(document.is_error);
    REQUIRE(document.params_json.find("REAL STANDALONE INSPECTOR WORKFLOW")
            != std::string::npos);
    REQUIRE(document.params_json.find("workflow-gain") != std::string::npos);
    const auto pre_mutation_dom = dom_value_for_id(
        choc::json::parse(document.params_json), "workflow-gain");
    REQUIRE(pre_mutation_dom.has_value());

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

    pulp::inspect::InspectorClient workflow_client;
    REQUIRE(workflow_client.connect(develop_record, reader));
    auto set_params = choc::value::createObject("");
    set_params.addMember("id", choc::value::createInt64(parameter_id));
    set_params.addMember("value", choc::value::createFloat64(target));
    set_params.addMember("normalized", choc::value::createBool(false));
    const auto set_result = controlled_request(
        workflow_client, "State.setParameter",
        choc::json::toString(set_params, false));
    INFO("set response=" << set_result.params_json);
    REQUIRE_FALSE(set_result.is_error);
    REQUIRE(choc::json::parse(set_result.params_json)["ok"].getBool());
    const double expected_normalized = (target - minimum) / (maximum - minimum);
    std::optional<double> mutated_dom;
    const auto mutated_dom_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < mutated_dom_deadline) {
        const auto response = client.request(
            "DOM.getDocument", "{}", std::chrono::seconds(2));
        if (!response.is_error) {
            mutated_dom = dom_value_for_id(
                choc::json::parse(response.params_json), "workflow-gain");
            if (mutated_dom
                && *mutated_dom
                    == Catch::Approx(expected_normalized).margin(0.001)) {
                break;
            }
        }
        wait_for_external_state_change_poll();
    }
    REQUIRE(mutated_dom.has_value());
    REQUIRE(*mutated_dom
            == Catch::Approx(expected_normalized).margin(0.001));

    const auto value_channels = workflow_client.request(
        "State.getValueChannels", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(value_channels.is_error);
    const auto value_channel_catalog = choc::json::parse(value_channels.params_json);
    REQUIRE(value_channel_catalog.isArray());
    REQUIRE(value_channel_catalog.size() == 4);

    bool saw_scalar = false;
    bool saw_vector = false;
    bool saw_events = false;
    bool saw_stale = false;
    std::string last_telemetry_snapshot;
    const auto telemetry_ready_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!(saw_scalar && saw_vector && saw_events && saw_stale)
           && std::chrono::steady_clock::now() < telemetry_ready_deadline) {
        const auto telemetry_snapshot = workflow_client.request(
            "Telemetry.getSnapshot", "{}", std::chrono::seconds(2));
        REQUIRE_FALSE(telemetry_snapshot.is_error);
        const auto telemetry_snapshot_json = choc::json::parse(
            telemetry_snapshot.params_json);
        last_telemetry_snapshot = telemetry_snapshot.params_json;
        REQUIRE(telemetry_snapshot_json["channels"].size() == 4);
        const auto snapshot_channels = telemetry_snapshot_json["channels"];
        for (std::uint32_t i = 0; i < snapshot_channels.size(); ++i) {
            const auto channel = snapshot_channels[i];
            const auto name = channel["name"].getString();
            saw_scalar |= name == "workflow_scalar" && channel["available"].getBool();
            saw_vector |= name == "workflow_vector" && channel["available"].getBool();
            saw_events |= name == "workflow_events" && channel["available"].getBool();
            saw_stale |= name == "workflow_stale" && channel["stale"].getBool();
        }
        wait_for_external_state_change_poll();
    }
    INFO("last telemetry snapshot=" << last_telemetry_snapshot);
    REQUIRE(saw_scalar);
    REQUIRE(saw_vector);
    REQUIRE(saw_events);
    REQUIRE(saw_stale);

    std::mutex telemetry_mutex;
    std::condition_variable telemetry_cv;
    std::vector<std::string> telemetry_samples;
    workflow_client.set_event_handler([&](const pulp::inspect::InspectorMessage& event) {
        if (event.method != "Telemetry.sample") return;
        {
            std::lock_guard lock(telemetry_mutex);
            telemetry_samples.push_back(event.params_json);
        }
        telemetry_cv.notify_all();
    });
    const auto subscription = workflow_client.request(
        "Telemetry.subscribe",
        R"({"channels":["workflow_scalar","workflow_vector","workflow_events","workflow_stale"],"rateHz":1,"maxVectorValues":4})",
        std::chrono::seconds(2));
    REQUIRE_FALSE(subscription.is_error);
    {
        std::unique_lock lock(telemetry_mutex);
        REQUIRE(telemetry_cv.wait_for(lock, std::chrono::seconds(4), [&] {
            return telemetry_samples.size() >= 2;
        }));
    }
    std::string first_sample_json;
    std::string slow_sample_json;
    {
        std::lock_guard lock(telemetry_mutex);
        first_sample_json = telemetry_samples.front();
        slow_sample_json = telemetry_samples.back();
    }
    auto first_sample = choc::json::parse(first_sample_json);
    auto slow_sample = choc::json::parse(slow_sample_json);
    REQUIRE(slow_sample["attemptSequence"].getInt64()
            > first_sample["attemptSequence"].getInt64());
    bool observed_source_drop = false;
    const auto slow_channels = slow_sample["channels"];
    for (std::uint32_t i = 0; i < slow_channels.size(); ++i) {
        const auto channel = slow_channels[i];
        if (channel["name"].getString() == "workflow_events") {
            observed_source_drop =
                channel["sourceDropped"].getWithDefault<std::int64_t>(0) > 0
                || channel["coalesced"].getWithDefault<std::int64_t>(0) > 0;
        }
    }
    REQUIRE(observed_source_drop);

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
        if (png.empty()) wait_for_external_state_change_poll();
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

    {
        std::ofstream script(develop_path / "standalone-workflow-ui.js", std::ios::app);
        script << "\nconsole.log(\"REAL RELOAD SCRIPT LOG\");\n"
                  "createLabel(\"workflow-reloaded\", \"REAL RELOAD COMPLETE\", \"root\");\n";
        REQUIRE(script.good());
    }
    {
        std::ofstream reload(reload_path);
        reload << "reload\n";
    }
    const auto pre_reload_attempt_sequence =
        slow_sample["attemptSequence"].getInt64();
    const auto pre_reload_source_generation =
        slow_sample["sourceGeneration"].getInt64();
    std::string post_reload_sample_json;
    {
        std::unique_lock lock(telemetry_mutex);
        REQUIRE(telemetry_cv.wait_for(lock, std::chrono::seconds(4), [&] {
            for (const auto& sample : telemetry_samples) {
                if (choc::json::parse(sample)["attemptSequence"]
                        .getWithDefault<std::int64_t>(0)
                    > pre_reload_attempt_sequence) {
                    post_reload_sample_json = sample;
                    return true;
                }
            }
            return false;
        }));
    }
    const auto post_reload_sample = choc::json::parse(post_reload_sample_json);
    REQUIRE_FALSE(post_reload_sample["reattached"].getWithDefault(false));
    REQUIRE(post_reload_sample["sourceGeneration"].getInt64()
            == pre_reload_source_generation);
    REQUIRE(post_reload_sample["channels"].size() == 4);

    const auto runtime_after_reload = client.request(
        "Runtime.getCapabilities", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(runtime_after_reload.is_error);
    REQUIRE(choc::json::parse(runtime_after_reload.params_json)
                ["canEvaluate"].getBool());
    const auto post_reload_controller = client.request(
        "Session.acquireController", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(post_reload_controller.is_error);
    const auto eval_after_reload = client.request(
        "Runtime.evaluate",
        R"({"code":"console.log('REAL POST RELOAD EVAL LOG'); 8 * 8"})",
        std::chrono::seconds(2));
    INFO("post-reload runtime eval response=" << eval_after_reload.params_json);
    REQUIRE_FALSE(eval_after_reload.is_error);
    REQUIRE(choc::json::parse(eval_after_reload.params_json)
                ["result"].getWithDefault<std::int64_t>(0) == 64);
    const auto post_reload_release = client.request(
        "Session.releaseController", "{}", std::chrono::seconds(2));
    REQUIRE_FALSE(post_reload_release.is_error);

    std::string reloaded_dom;
    std::string reloaded_logs;
    const auto reloaded_sources_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while ((reloaded_dom.find("REAL RELOAD COMPLETE") == std::string::npos
            || reloaded_logs.find("REAL POST RELOAD EVAL LOG") == std::string::npos)
           && std::chrono::steady_clock::now() < reloaded_sources_deadline) {
        const auto dom = client.request(
            "DOM.getDocument", "{}", std::chrono::seconds(2));
        if (!dom.is_error) reloaded_dom = dom.params_json;
        const auto logs = client.request(
            "Console.getMessages", "{}", std::chrono::seconds(2));
        if (!logs.is_error) reloaded_logs = logs.params_json;
        if (reloaded_dom.find("REAL RELOAD COMPLETE") == std::string::npos
            || reloaded_logs.find("REAL POST RELOAD EVAL LOG") == std::string::npos)
            wait_for_external_state_change_poll();
    }
    REQUIRE(reloaded_dom.find("REAL RELOAD COMPLETE") != std::string::npos);
    const auto post_reload_log = reloaded_logs.find("REAL POST RELOAD EVAL LOG");
    REQUIRE(post_reload_log != std::string::npos);
    REQUIRE(reloaded_logs.find("REAL POST RELOAD EVAL LOG", post_reload_log + 1)
            == std::string::npos);

    pulp::inspect::StandaloneTransportTestInput transport_input;
    transport_input.playing = false;
    transport_input.position_samples = 96'000;
    transport_input.tempo_bpm = 90.0;
    auto transport_params = choc::value::createObject("");
    transport_params.addMember("playing", choc::value::createBool(false));
    transport_params.addMember("position_samples", choc::value::createInt64(96'000));
    transport_params.addMember("tempo_bpm", choc::value::createFloat64(90.0));
    const auto transport_result = controlled_request(
        workflow_client, "Test.setTransport",
        choc::json::toString(transport_params, false));
    INFO("transport response=" << transport_result.params_json);
    REQUIRE_FALSE(transport_result.is_error);
    REQUIRE(choc::json::parse(transport_result.params_json)["applied"].getBool());

    auto midi_params = choc::value::createObject("");
    midi_params.addMember("kind", choc::value::createString("note_on"));
    midi_params.addMember("channel", choc::value::createInt32(3));
    midi_params.addMember("note", choc::value::createInt32(64));
    midi_params.addMember("velocity", choc::value::createInt32(99));
    const auto midi_result = controlled_request(
        workflow_client, "Test.injectMidi",
        choc::json::toString(midi_params, false));
    INFO("MIDI response=" << midi_result.params_json);
    REQUIRE_FALSE(midi_result.is_error);
    REQUIRE(choc::json::parse(midi_result.params_json)["accepted"].getBool());

    const auto observation_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(observation_path) &&
           std::chrono::steady_clock::now() < observation_deadline) {
        wait_for_external_state_change_poll();
    }
    REQUIRE(std::filesystem::exists(observation_path));
    std::ifstream observation_input(observation_path);
    const auto observation_json = choc::json::parse(
        std::string(std::istreambuf_iterator<char>(observation_input), {}));
    REQUIRE(observation_json["note_on_count"].getInt64() > 0);
    REQUIRE(observation_json["note_off_count"].getInt64() > 0);
    REQUIRE(observation_json["transport_match"].getBool());

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

    const auto record_path = develop_record.record_path;
    const auto credential_path = develop_record.credential_path;
    auto ownership_path = record_path;
    ownership_path.replace_extension(".lock");
    const auto observe_record_path = observe_record.record_path;
    const auto observe_credential_path = observe_record.credential_path;
    auto observe_ownership_path = observe_record_path;
    observe_ownership_path.replace_extension(".lock");
    const auto eval_record_path = eval_record.record_path;
    const auto eval_credential_path = eval_record.credential_path;
    auto eval_ownership_path = eval_record_path;
    eval_ownership_path.replace_extension(".lock");
    workflow_client.disconnect();
    client.disconnect();
    observe_client.disconnect();
    eval_client.disconnect();
    {
        std::ofstream stop(stop_path);
        stop << "stop\n";
    }
    {
        std::ofstream stop(eval_stop_path);
        stop << "stop\n";
    }
    {
        std::ofstream stop(observe_stop_path);
        stop << "stop\n";
    }
    const auto result = child.wait();
    const auto observe_result = observe_child.wait();
    const auto eval_result = eval_child.wait();
    INFO("stdout=" << result.stdout_output);
    INFO("stderr=" << result.stderr_output);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(observe_result.timed_out);
    REQUIRE(observe_result.exit_code == 0);
    REQUIRE_FALSE(eval_result.timed_out);
    REQUIRE(eval_result.exit_code == 0);
    REQUIRE_FALSE(std::filesystem::exists(record_path));
    REQUIRE_FALSE(std::filesystem::exists(credential_path));
    REQUIRE(std::filesystem::is_regular_file(ownership_path));
    REQUIRE_FALSE(std::filesystem::exists(observe_record_path));
    REQUIRE_FALSE(std::filesystem::exists(observe_credential_path));
    REQUIRE(std::filesystem::is_regular_file(observe_ownership_path));
    REQUIRE_FALSE(std::filesystem::exists(eval_record_path));
    REQUIRE_FALSE(std::filesystem::exists(eval_credential_path));
    REQUIRE(std::filesystem::is_regular_file(eval_ownership_path));
    REQUIRE(runtime_contains_only_inert_locks(
        runtime_path, {ownership_path, observe_ownership_path, eval_ownership_path}));
    REQUIRE(reader.list().empty());
}
