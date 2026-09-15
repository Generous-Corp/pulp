#include <catch2/catch_test_macros.hpp>
#include <pulp/inspect/agent_context.hpp>
#include <pulp/inspect/capture_source.hpp>
#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/skp_capture_source.hpp>

#include <choc/text/choc_JSON.h>

using namespace pulp::inspect;

namespace {

class TestAgentContextSource final : public InspectorAgentContextSource {
public:
    bool hot_reload_available = true;
    bool hot_reload_enabled = true;
    bool hot_reload_pending = false;

    InspectorAgentContext snapshot() const override {
        return {
            .binary_path = "/tmp/PulpGain",
            .binary_build_id = "build-abc",
            .binary_mtime_unix_ms = 123456,
            .plugin_id = "com.pulp.gain",
            .session_id = "session-a",
            .instance_id = "instance-b",
            .editor_open = true,
            .window_visible = true,
            .processing = true,
            .xrun_count = 3,
            .hot_reload_available = hot_reload_available,
            .hot_reload_enabled = hot_reload_enabled,
            .hot_reload_pending = hot_reload_pending,
            .unsaved_tweak_count = 2,
            .actionable_issues = {"audio device changed"},
        };
    }
};

class TestCaptureSource final : public InspectorCaptureSource {
public:
    InspectorCapture capture_png() override {
        ++calls;
        if (!error.empty())
            return {{}, 0, 0, error, error_code};
        return {{0x89, 0x50, 0x4e, 0x47}, 320, 200, {}, {}};
    }

    int calls = 0;
    std::string error;
    std::string error_code;
};

class TestSkpCaptureSource final : public SkpCaptureSource {
public:
    InspectorSkpCapture capture_skp() override {
        ++calls;
        if (!error.empty())
            return {{}, 0, 0, 0, error, error_code};
        if (empty_result)
            return {{}, 800, 600, 0, {}, {}};
        return {{0x73, 0x6b, 0x69, 0x61, 0x70, 0x69, 0x63, 0x74}, 800, 600, 42, {}, {}};
    }

    int calls = 0;
    bool empty_result = false;
    std::string error;
    std::string error_code;
};

} // namespace

TEST_CASE("DomainHandler: agent context is typed and host-owned",
          "[inspect][domain][agent-context]") {
    DomainHandler handler;
    auto unavailable = handler.handle(
        make_request(1, methods::kInspectorGetAgentContext));
    REQUIRE(unavailable.is_error);
    REQUIRE(unavailable.error_code == "context_unavailable");

    TestAgentContextSource source;
    handler.set_agent_context_source(&source);
    auto response = handler.handle(
        make_request(2, methods::kInspectorGetAgentContext));
    REQUIRE_FALSE(response.is_error);
    const auto value = choc::json::parse(response.params_json);
    REQUIRE(value["binary"]["path"].getString() == "/tmp/PulpGain");
    REQUIRE(value["binary"]["buildId"].getString() == "build-abc");
    REQUIRE(value["binary"]["mtimeUnixMs"].getInt64() == 123456);
    REQUIRE(value["identity"]["sessionId"].getString() == "session-a");
    REQUIRE(value["identity"]["instanceId"].getString() == "instance-b");
    REQUIRE(value["editor"]["windowVisible"].getBool());
    REQUIRE(value["processing"]["active"].getBool());
    REQUIRE(value["processing"]["xrunCount"].getInt64() == 3);
    REQUIRE(value["hotReload"]["available"].getBool());
    REQUIRE(value["hotReload"]["enabled"].getBool());
    REQUIRE_FALSE(value["hotReload"]["pending"].getBool());
    REQUIRE(value["unsavedTweakCount"].getInt64() == 2);
    REQUIRE(value["actionableIssues"][0].getString()
            == "audio device changed");
    const auto first_issues = response.params_json.find("\"actionableIssues\"");
    REQUIRE(first_issues != std::string::npos);
    REQUIRE(response.params_json.find("\"actionableIssues\"", first_issues + 1) ==
            std::string::npos);

    auto hot_reload = handler.handle(
        make_request(3, methods::kRuntimeGetHotReloadStatus));
    REQUIRE_FALSE(hot_reload.is_error);
    const auto reload = choc::json::parse(hot_reload.params_json);
    REQUIRE(reload["available"].getBool());
    REQUIRE(reload["enabled"].getBool());
    REQUIRE_FALSE(reload["pending"].getBool());

    source.hot_reload_available = false;
    source.hot_reload_enabled = false;
    auto native_reload = handler.handle(
        make_request(4, methods::kRuntimeGetHotReloadStatus));
    REQUIRE_FALSE(native_reload.is_error);
    const auto native = choc::json::parse(native_reload.params_json);
    REQUIRE_FALSE(native["available"].getBool());
    REQUIRE_FALSE(native["enabled"].getBool());
    REQUIRE_FALSE(native["pending"].getBool());
}

TEST_CASE("DomainHandler: screenshot uses the selected host capture seam",
          "[inspect][domain][capture]") {
    DomainHandler handler;
    auto unavailable = handler.handle(
        make_request(1, methods::kCaptureScreenshot));
    REQUIRE(unavailable.is_error);
    REQUIRE(unavailable.error_code == "capture_unavailable");

    TestCaptureSource source;
    handler.set_capture_source(&source);
    auto response = handler.handle(
        make_request(2, methods::kCaptureScreenshot));
    REQUIRE_FALSE(response.is_error);
    REQUIRE(source.calls == 1);
    const auto value = choc::json::parse(response.params_json);
    REQUIRE(value["mimeType"].getString() == "image/png");
    REQUIRE(value["width"].getInt64() == 320);
    REQUIRE(value["height"].getInt64() == 200);
    REQUIRE(value["data"].getString() == "iVBORw==");

    source.error = "current view contains a native overlay";
    source.error_code = "capture_unavailable";
    auto dynamic_unavailable = handler.handle(
        make_request(3, methods::kCaptureScreenshot));
    REQUIRE(dynamic_unavailable.is_error);
    REQUIRE(dynamic_unavailable.error_code == "capture_unavailable");

    auto node = handler.handle(
        make_request(4, methods::kCaptureScreenshotNode));
    REQUIRE(node.is_error);
    REQUIRE(node.error_code == "method_unavailable");
}

TEST_CASE("DomainHandler: frame capture uses the selected host .skp seam",
          "[inspect][domain][capture]") {
    DomainHandler handler;
    auto unavailable = handler.handle(
        make_request(1, methods::kRenderCaptureFrame));
    REQUIRE(unavailable.is_error);
    REQUIRE(unavailable.error_code == "capture_unavailable");

    TestSkpCaptureSource source;
    handler.set_skp_capture_source(&source);
    auto response = handler.handle(
        make_request(2, methods::kRenderCaptureFrame));
    REQUIRE_FALSE(response.is_error);
    REQUIRE(source.calls == 1);
    const auto value = choc::json::parse(response.params_json);
    REQUIRE(value["mimeType"].getString() == "application/x-skia-picture");
    REQUIRE(value["width"].getInt64() == 800);
    REQUIRE(value["height"].getInt64() == 600);
    REQUIRE(value["opCount"].getInt64() == 42);
    REQUIRE(value["data"].getString() == "c2tpYXBpY3Q=");

    source.error = "frame contains a protected surface";
    source.error_code = "capture_unavailable";
    auto dynamic_unavailable = handler.handle(
        make_request(3, methods::kRenderCaptureFrame));
    REQUIRE(dynamic_unavailable.is_error);
    REQUIRE(dynamic_unavailable.error_code == "capture_unavailable");

    // A source that reports no error but hands back nothing is a failure, not
    // an empty artifact.
    source.error.clear();
    source.error_code.clear();
    source.empty_result = true;
    auto empty = handler.handle(make_request(4, methods::kRenderCaptureFrame));
    REQUIRE(empty.is_error);
    REQUIRE(empty.error_code == "capture_failed");
    source.empty_result = false;

    auto unknown = handler.handle(make_request(5, "Render.nope"));
    REQUIRE(unknown.is_error);
}
