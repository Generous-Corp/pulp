// test_editor_bridge_core.cpp: EditorBridge's dispatch core, linked against
// pulp::editor-bridge alone.
//
// The target links pulp::editor-bridge and nothing else, so these cases prove
// that an SDK without Pulp's view libraries (one that serves its UI names from
// Vellum) still gets a working bridge, and that the bridge's include closure
// exposes none of Pulp's legacy view headers next to Vellum's. The header must
// declare only what the library defines: every member is called below, so a
// declaration without a definition fails this target's link, and the attach
// members that need view types must not be declared at all.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/editor_bridge.hpp>

#include <choc/containers/choc_Value.h>

#include <string>
#include <vector>

// pulp/view/view.hpp is the root of Pulp's legacy view layer. Reaching it from
// this target would mean the bridge's include path carries core/view.
#if __has_include(<pulp/view/view.hpp>)
#error "pulp::editor-bridge must not put Pulp's view headers on the include path"
#endif

using pulp::view::EditorBridge;

namespace pulp::view {
class WebViewPanel;
class ScriptEngine;
class ScriptedUiSession;
class JsRuntime;
} // namespace pulp::view

// The view-side attachments live in pulp/view/editor_bridge_attach.hpp, which
// pulp::view-tooling defines. A member of the same name here would compile in a
// core-only consumer and fail only at link time.
namespace {
template <class Bridge>
concept HasViewAttachments =
    requires(Bridge& b, pulp::view::WebViewPanel& p) { b.attach_webview(p); } ||
    requires(Bridge& b, pulp::view::WebViewPanel& p) { b.detach_webview(p); } ||
    requires(Bridge& b, pulp::view::JsRuntime& r) { b.attach_native_runtime(r, "x"); } ||
    requires(Bridge& b, pulp::view::ScriptEngine& e) { b.attach_native_runtime(e, "x"); } ||
    requires(Bridge& b, pulp::view::ScriptedUiSession& s) { b.attach_native_runtime(s, "x"); } ||
    requires(Bridge& b, pulp::view::ScriptedUiSession& s) { b.detach_native_runtime(s, "x"); };
} // namespace

static_assert(!HasViewAttachments<EditorBridge>,
              "EditorBridge's header must not declare the view-side attachments");

TEST_CASE("EditorBridge core dispatches a registered handler",
          "[editor_bridge][editor-bridge-core]") {
    EditorBridge bridge;
    int calls = 0;
    bridge.add_handler("ping", [&calls](const choc::value::ValueView&) {
        ++calls;
        return EditorBridge::ok_response();
    });
    CHECK(bridge.dispatch_json(R"({"type":"ping"})") == EditorBridge::ok_response());
    CHECK(calls == 1);
}

TEST_CASE("EditorBridge core reports an unknown message type",
          "[editor_bridge][editor-bridge-core]") {
    EditorBridge bridge;
    const std::string reply = bridge.dispatch_json(R"({"type":"missing"})");
    CHECK(reply.find("unknown message type") != std::string::npos);
}

TEST_CASE("EditorBridge core defines every function its header declares",
          "[editor_bridge][editor-bridge-core]") {
    EditorBridge bridge;
    bridge.add_handler("set", [](const choc::value::ValueView& payload) {
        auto extras = choc::value::createObject("");
        extras.addMember("v", EditorBridge::get_float(payload, "v", 0.0f));
        extras.addMember("n", static_cast<int64_t>(EditorBridge::get_uint(payload, "n", 0)));
        extras.addMember("s", EditorBridge::get_string(payload, "s"));
        return EditorBridge::ok_response(extras);
    });
    CHECK(bridge.has_handler("set"));
    CHECK(bridge.handler_count() == 1);
    CHECK(bridge.handlers() == std::vector<std::string>{"set"});

    auto payload = choc::value::createObject("");
    payload.addMember("v", 0.5);
    payload.addMember("n", static_cast<int64_t>(3));
    payload.addMember("s", "x");
    const std::string direct = bridge.dispatch("set", payload);
    CHECK(direct.find("\"ok\"") != std::string::npos);
    CHECK(direct.find("\"s\"") != std::string::npos);
    CHECK(direct.find("error") == std::string::npos);
    CHECK(bridge.dispatch_webview_message("set", R"({"v":0.5,"n":3,"s":"x"})") == direct);
    CHECK(bridge.dispatch_json(R"({"type":"set","payload":{"v":0.5,"n":3,"s":"x"}})") == direct);

    bridge.remove_handler("set");
    CHECK_FALSE(bridge.has_handler("set"));
    CHECK(EditorBridge::err_response("bad").find("bad") != std::string::npos);
}
