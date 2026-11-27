// test_editor_bridge_core.cpp: EditorBridge's dispatch core, linked against
// pulp::editor-bridge alone.
//
// The target links pulp::editor-bridge and nothing else, so these cases prove
// that an SDK without Pulp's view libraries (one that serves its UI names from
// Vellum) still gets a working bridge, and that the bridge's include closure
// exposes none of Pulp's legacy view headers next to Vellum's. Every
// non-template function the header declares is called below, so a declaration
// the library does not define fails this target's link. The deprecated
// attachment members are templates that forward to editor_bridge_attach.hpp;
// test_editor_bridge_forwarders.cmake proves a use without it fails to compile.

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
