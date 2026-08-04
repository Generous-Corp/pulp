#include <catch2/catch_test_macros.hpp>
#include <pulp/format/detail/standalone_inspector.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/window_host.hpp>
#include "support/standalone_inspector_test_support.hpp"

#include <choc/text/choc_JSON.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <vector>

using namespace pulp::format;
using namespace pulp::format::detail;
using namespace pulp::view;
using namespace pulp::test::standalone_inspector;

TEST_CASE("Standalone inspector runtime evaluation requires an active controller profile",
          "[standalone][inspect][runtime-eval][negative]") {
    StandaloneApp app(null_processor_factory);
    TestProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    View root;
    StubWindowHost window;

    REQUIRE(StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "off", {}, true) == nullptr);
    REQUIRE(StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "observe", {}, true) == nullptr);
    REQUIRE(StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "custom",
        {"session.control", "runtime.eval"}) == nullptr);
    REQUIRE(StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "custom",
        {"runtime.eval"}, true) == nullptr);
    REQUIRE(StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "custom",
        {"session.describe", "session.control"}, true) == nullptr);

    auto develop = StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "develop", {}, true);
    REQUIRE(develop != nullptr);
}

TEST_CASE("Standalone inspector runtime evaluation rejects every effectful live-realm grant",
          "[standalone][inspect][runtime-eval][capabilities][negative]") {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path()
        / ("pulp-standalone-inspector-eval-grants-" + suffix);
    const auto script = temp / "ui.js";
    std::filesystem::create_directories(temp);
    {
        std::ofstream source(script);
        source << "createLabel('v', 'safe fixture', '');";
    }

    constexpr std::array effectful{
        ReloadCapability::Exec,
        ReloadCapability::Clipboard,
        ReloadCapability::Filesystem,
        ReloadCapability::Storage,
        ReloadCapability::Ai,
        ReloadCapability::RuntimeImport,
        ReloadCapability::Network,
    };
    for (const auto capability : effectful) {
        DYNAMIC_SECTION("grant=" << capability_name(capability)) {
            StandaloneApp app(null_processor_factory);
            CapabilitySet granted;
            granted.grant(capability);
            InspectorProcessor processor(app.state(), script, granted);
            ViewBridge bridge(processor, app.state());
            REQUIRE(bridge.open());
            REQUIRE(processor.active_scripted_ui()->granted_capabilities().has(capability));
            REQUIRE(processor.active_scripted_ui()->bridge()
                        ->granted_capabilities().has(capability));

            const auto expected =
                "Runtime.evaluate denied: live scripted-UI realm grants effectful capability '" +
                std::string(capability_name(capability)) + "'";
            REQUIRE(standalone_runtime_eval_realm_denial(
                        processor.active_scripted_ui()) == expected);

            StubWindowHost window;
            REQUIRE(StandaloneInspectorRuntime::create(
                        app, processor, bridge, *bridge.view(), window,
                        "develop", {}, true) == nullptr);
            // The refusal observes the actual realm; it never mutates its grant
            // set or masks the corresponding native API registration.
            REQUIRE(processor.active_scripted_ui()->bridge()
                        ->granted_capabilities().has(capability));
            processor.close_editor(bridge);
        }
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(temp, cleanup_error);
}

TEST_CASE("Standalone inspector runtime evaluation rejects a tree beyond its bounded reset",
          "[standalone][inspect][runtime-eval][reset][negative]") {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path()
        / ("pulp-standalone-inspector-eval-reset-" + suffix);
    const auto script = temp / "ui.js";
    std::filesystem::create_directories(temp);
    {
        std::ofstream source(script);
        source << "for (let i = 0; i < 2049; ++i) "
                  "createLabel('v' + i, 'safe', '');";
    }

    StandaloneApp app(null_processor_factory);
    CapabilitySet safe;
    InspectorProcessor processor(app.state(), script, safe);
    ViewBridge bridge(processor, app.state());
    REQUIRE(bridge.open());

    const std::string denial =
        "Runtime.evaluate denied: live scripted-UI realm reset tree exceeds bounded cleanup limit";
    REQUIRE(standalone_runtime_eval_realm_denial(
                processor.active_scripted_ui()) == denial);

    StubWindowHost window;
    REQUIRE(StandaloneInspectorRuntime::create(
                app, processor, bridge, *bridge.view(), window,
                "develop", {}, true) == nullptr);
    REQUIRE(processor.active_scripted_ui()->bridge() != nullptr);

    processor.close_editor(bridge);
    std::error_code cleanup_error;
    std::filesystem::remove_all(temp, cleanup_error);
}

TEST_CASE("Standalone inspector runtime evaluation survives safe reload and refuses unsafe rebind",
          "[standalone][inspect][runtime-eval][capabilities][reload]") {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path()
        / ("pulp-standalone-inspector-eval-rebind-" + suffix);
    const auto runtime_dir = temp / "runtime";
    const auto script = temp / "ui.js";
    std::filesystem::create_directories(temp);
    {
        std::ofstream source(script);
        source << "globalThis.fixtureVersion = 1;"
                  "globalThis.deferredRan = false;"
                  "createLabel('v', 'safe', '');";
    }
    ScopedEnv runtime_env("PULP_INSPECTOR_RUNTIME_DIR");
    runtime_env.set(runtime_dir.string());

    StandaloneApp app(null_processor_factory);
    CapabilitySet safe;
    NonReloadingInspectorProcessor processor(app.state(), script, safe);
    ViewBridge bridge(processor, app.state());
    REQUIRE(bridge.open());
    REQUIRE(processor.active_scripted_ui()->granted_capabilities().empty());
    StubWindowHost window;
    QueuedMainThreadBackend dispatcher;
    REQUIRE(dispatcher.valid());
    auto runtime = StandaloneInspectorRuntime::create(
        app, processor, bridge, *bridge.view(), window, "develop", {}, true);
    REQUIRE(runtime != nullptr);
    runtime->pump();

    pulp::inspect::InspectorDiscoveryReader reader(runtime_dir);
    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    pulp::inspect::InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto request = [&](std::string method, std::string params) {
        return request_with_dispatch(client, dispatcher,
                                     std::move(method), std::move(params));
    };
    REQUIRE_FALSE(request("Session.acquireController", "{}").is_error);

    auto capabilities = request("Runtime.getCapabilities", "{}");
    REQUIRE_FALSE(capabilities.is_error);
    REQUIRE(choc::json::parse(capabilities.params_json)
                ["canEvaluate"].getWithDefault(false));
    auto evaluated = request("Runtime.evaluate", R"({"code":"fixtureVersion + 1"})");
    REQUIRE_FALSE(evaluated.is_error);
    REQUIRE(choc::json::parse(evaluated.params_json)
                ["result"].getWithDefault<std::int64_t>(0) == 2);

    auto interrupted_evaluation = std::async(std::launch::async, [&] {
        return client.request(
            "Runtime.evaluate", R"({"code":"while (true) {}"})",
            std::chrono::seconds(4));
    });
    REQUIRE(spin_until([&] { return dispatcher.pending_count() == 1; }));
    auto interruption = std::async(std::launch::async, [&] {
        if (!spin_until([&] {
                return processor.active_scripted_ui()
                    ->script_inspector()->is_busy();
            }, std::chrono::seconds(1))) {
            return pulp::inspect::make_error(
                0, "evaluation never entered its single-flight slot");
        }
        return client.request("Runtime.interrupt", "{}",
                              std::chrono::seconds(1));
    });
    REQUIRE(dispatcher.pump_one());
    REQUIRE(interruption.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
    const auto interruption_response = interruption.get();
    REQUIRE_FALSE(interruption_response.is_error);
    REQUIRE(choc::json::parse(interruption_response.params_json)
                ["interrupted"].getWithDefault(false));
    REQUIRE(interrupted_evaluation.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
    const auto interrupted_response = interrupted_evaluation.get();
    REQUIRE(interrupted_response.is_error);
    REQUIRE(interrupted_response.params_json.find("evaluation interrupted") !=
            std::string::npos);

    auto timed_evaluation = std::async(std::launch::async, [&] {
        return client.request(
            "Runtime.evaluate", R"({"code":"while (true) {}"})",
            std::chrono::seconds(4));
    });
    REQUIRE(spin_until([&] { return dispatcher.pending_count() == 1; }));
    const auto timeout_started = std::chrono::steady_clock::now();
    REQUIRE(dispatcher.pump_one());
    const auto timeout_elapsed =
        std::chrono::steady_clock::now() - timeout_started;
    REQUIRE(timed_evaluation.wait_for(std::chrono::seconds(1)) ==
            std::future_status::ready);
    const auto timeout_response = timed_evaluation.get();
    REQUIRE(timeout_response.is_error);
    REQUIRE(timeout_response.error_code != "main_thread_timeout");
    REQUIRE(timeout_response.params_json.find("Runtime.evaluate timed out") !=
            std::string::npos);
    REQUIRE(timeout_elapsed < std::chrono::seconds(3));

    evaluated = request(
        "Runtime.evaluate",
        R"({"code":"globalThis.transientMarker = 99; setTimeout(() => { globalThis.deferredRan = true; }, 0); Promise.resolve().then(() => { globalThis.deferredRan = true; }); 1"})");
    REQUIRE_FALSE(evaluated.is_error);
    evaluated = request(
        "Runtime.evaluate",
        R"json({"code":"({ deferredRan: globalThis.deferredRan, hasMarker: typeof globalThis.transientMarker !== 'undefined' })"})json");
    REQUIRE_FALSE(evaluated.is_error);
    const auto reset_response = choc::json::parse(evaluated.params_json);
    const auto reset_probe = reset_response["result"];
    REQUIRE_FALSE(reset_probe["deferredRan"].getWithDefault(true));
    REQUIRE_FALSE(reset_probe["hasMarker"].getWithDefault(true));

    {
        std::ofstream source(script);
        source << "globalThis.fixtureVersion = 2;"
                  "globalThis.deferredRan = false;"
                  "createLabel('v', 'reloaded', '');";
    }
    std::string reload_error;
    REQUIRE(processor.active_scripted_ui()->reload(&reload_error));
    REQUIRE(processor.active_scripted_ui()->granted_capabilities().empty());
    REQUIRE(processor.active_scripted_ui()->bridge()
                ->granted_capabilities().empty());
    evaluated = request("Runtime.evaluate", R"({"code":"fixtureVersion + 1"})");
    REQUIRE_FALSE(evaluated.is_error);
    REQUIRE(choc::json::parse(evaluated.params_json)
                ["result"].getWithDefault<std::int64_t>(0) == 3);

    CapabilitySet unsafe;
    unsafe.grant(ReloadCapability::Exec);
    REQUIRE(processor.replace_scripted_ui(unsafe));
    capabilities = request("Runtime.getCapabilities", "{}");
    REQUIRE_FALSE(capabilities.is_error);
    const auto unsafe_caps = choc::json::parse(capabilities.params_json);
    REQUIRE_FALSE(unsafe_caps["canEvaluate"].getWithDefault(true));
    const std::string denial =
        "Runtime.evaluate denied: live scripted-UI realm grants effectful capability 'exec'";
    REQUIRE(unsafe_caps["evaluateDeniedReason"].toString() == denial);
    const auto denied = request("Runtime.evaluate", R"({"code":"1 + 1"})");
    REQUIRE(denied.is_error);
    REQUIRE(denied.params_json.find(denial) != std::string::npos);
    REQUIRE(processor.active_scripted_ui()->bridge()
                ->granted_capabilities().has(ReloadCapability::Exec));

    REQUIRE(processor.replace_scripted_ui(safe));
    capabilities = request("Runtime.getCapabilities", "{}");
    REQUIRE(choc::json::parse(capabilities.params_json)
                ["canEvaluate"].getWithDefault(false));
    REQUIRE_FALSE(request("Runtime.evaluate", R"({"code":"6 * 7"})").is_error);

    client.disconnect();
    runtime->stop();
    runtime.reset();
    processor.close_editor(bridge);
    std::error_code cleanup_error;
    std::filesystem::remove_all(temp, cleanup_error);
}
