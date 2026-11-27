// test_editor_bridge_core.cpp: EditorBridge's dispatch core, linked against
// pulp::editor-bridge alone.
//
// The target links pulp::editor-bridge and nothing else, so these cases prove
// that an SDK without Pulp's view libraries (one that serves its UI names from
// Vellum) still gets a working bridge, and that the bridge's include closure
// exposes none of Pulp's legacy view headers next to Vellum's.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/editor_bridge.hpp>

#include <string>

// pulp/view/view.hpp is the root of Pulp's legacy view layer. Reaching it from
// this target would mean the bridge's include path carries core/view.
#if __has_include(<pulp/view/view.hpp>)
#error "pulp::editor-bridge must not put Pulp's view headers on the include path"
#endif

using pulp::view::EditorBridge;

TEST_CASE("EditorBridge core dispatches a registered handler", "[editor_bridge][editor-bridge-core]") {
    EditorBridge bridge;
    int calls = 0;
    bridge.add_handler("ping", [&calls](const choc::value::ValueView&) {
        ++calls;
        return EditorBridge::ok_response();
    });
    CHECK(bridge.dispatch_json(R"({"type":"ping"})") == EditorBridge::ok_response());
    CHECK(calls == 1);
}

TEST_CASE("EditorBridge core reports an unknown message type", "[editor_bridge][editor-bridge-core]") {
    EditorBridge bridge;
    const std::string reply = bridge.dispatch_json(R"({"type":"missing"})");
    CHECK(reply.find("unknown message type") != std::string::npos);
}
