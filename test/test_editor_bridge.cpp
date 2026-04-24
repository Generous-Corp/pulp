// pulp #709 — renderer-agnostic editor↔processor message bridge.
//
// Exercises every documented behavior of pulp::view::EditorBridge
// without touching a real WebView or JS runtime: envelope parse, the
// five standard error categories (malformed_json, unknown_type,
// missing_field, wrong_type, internal_error), value-coercion helpers,
// and response-builder shapes.
//
// Substring-compatibility note: the on-the-wire error strings here
// match Spectr's existing editor_bridge tests so Spectr's 110-test
// suite continues to pass when Spectr cuts over (#709 acceptance
// criterion).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "pulp/view/editor_bridge.hpp"
#include "pulp/view/web_view.hpp"

#include <choc/containers/choc_Value.h>
#include <choc/text/choc_JSON.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// Concrete JsRuntime definition so attach_native_runtime can link in
// tests. The framework forward-declares JsRuntime; the stub body
// never touches members, so an empty class satisfies the reference.
namespace pulp::view { class JsRuntime {}; }

using Catch::Approx;
using pulp::view::EditorBridge;

namespace {

bool response_ok(const std::string& r) {
    return r.find(R"("ok": true)") != std::string::npos
        || r.find(R"("ok":true)")  != std::string::npos;
}

bool response_has_error(const std::string& r, std::string_view substr) {
    const bool not_ok = r.find(R"("ok": false)") != std::string::npos
                     || r.find(R"("ok":false)")  != std::string::npos;
    return not_ok && r.find(substr) != std::string::npos;
}

} // namespace

// ── Envelope-level error vocabulary (matches #709 standard codes) ────────

TEST_CASE("EditorBridge: malformed JSON returns malformed_json error",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    const auto resp = bridge.dispatch_json("not json");
    CHECK(response_has_error(resp, "malformed JSON"));
}

TEST_CASE("EditorBridge: non-object envelope is rejected",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    const auto resp = bridge.dispatch_json("[1,2,3]");
    CHECK(response_has_error(resp, "envelope must be an object"));
}

TEST_CASE("EditorBridge: envelope missing 'type' returns missing_field error",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    const auto resp = bridge.dispatch_json(R"({"payload":{}})");
    CHECK(response_has_error(resp, "'type'"));
}

TEST_CASE("EditorBridge: envelope with non-string 'type' is rejected",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    const auto resp = bridge.dispatch_json(R"({"type":42})");
    CHECK(response_has_error(resp, "'type'"));
}

TEST_CASE("EditorBridge: envelope with empty-string 'type' is rejected",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    const auto resp = bridge.dispatch_json(R"({"type":""})");
    CHECK(response_has_error(resp, "'type'"));
}

TEST_CASE("EditorBridge: unknown type returns unknown_type error",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    const auto resp = bridge.dispatch_json(R"({"type":"not_registered"})");
    CHECK(response_has_error(resp, "unknown message type"));
}

TEST_CASE("EditorBridge: handler exception is caught as internal_error",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    bridge.add_handler("boom", [](const auto&) -> std::string {
        throw std::runtime_error("kaboom");
    });
    const auto resp = bridge.dispatch_json(R"({"type":"boom"})");
    CHECK(response_has_error(resp, "internal error"));
}

// ── Handler dispatch happy paths ─────────────────────────────────────────

TEST_CASE("EditorBridge: registered handler receives empty payload when omitted",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    bool called = false;
    bool was_object = false;
    bridge.add_handler("ping", [&](const choc::value::ValueView& payload) {
        called = true;
        was_object = payload.isObject();
        return EditorBridge::ok_response();
    });
    const auto resp = bridge.dispatch_json(R"({"type":"ping"})");
    CHECK(response_ok(resp));
    CHECK(called);
    CHECK(was_object);
}

TEST_CASE("EditorBridge: registered handler receives parsed payload",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    float observed = 0.0f;
    bridge.add_handler("morph", [&](const choc::value::ValueView& payload) {
        observed = EditorBridge::get_float(payload, "t", -1.0f);
        return EditorBridge::ok_response();
    });
    const auto resp = bridge.dispatch_json(R"({"type":"morph","payload":{"t":0.75}})");
    CHECK(response_ok(resp));
    CHECK(observed == Approx(0.75f));
}

TEST_CASE("EditorBridge: handler can return success with extras",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    bridge.add_handler("save_preset", [](const auto&) {
        auto extras = choc::value::createObject("");
        extras.addMember("preset_json", std::string{R"({"format":"pulp.preset"})"});
        return EditorBridge::ok_response(extras);
    });
    const auto resp = bridge.dispatch_json(R"({"type":"save_preset"})");
    CHECK(response_ok(resp));
    CHECK(resp.find("preset_json") != std::string::npos);
    CHECK(resp.find("pulp.preset") != std::string::npos);
}

TEST_CASE("EditorBridge: handler-level err_response surfaces the message verbatim",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    bridge.add_handler("paint", [](const auto&) {
        return EditorBridge::err_response("paint without paint_start");
    });
    const auto resp = bridge.dispatch_json(R"({"type":"paint"})");
    CHECK(response_has_error(resp, "paint without paint_start"));
}

// ── Handler registration semantics ───────────────────────────────────────

TEST_CASE("EditorBridge: add_handler replaces an existing handler silently",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    int chosen = 0;
    bridge.add_handler("hello", [&](const auto&) { chosen = 1; return EditorBridge::ok_response(); });
    bridge.add_handler("hello", [&](const auto&) { chosen = 2; return EditorBridge::ok_response(); });
    CHECK(bridge.handler_count() == 1);
    CHECK(bridge.has_handler("hello"));
    bridge.dispatch_json(R"({"type":"hello"})");
    CHECK(chosen == 2);
}

TEST_CASE("EditorBridge: remove_handler reverts to unknown_type",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    bridge.add_handler("hello", [](const auto&) { return EditorBridge::ok_response(); });
    bridge.remove_handler("hello");
    CHECK_FALSE(bridge.has_handler("hello"));
    CHECK(bridge.handler_count() == 0);
    const auto resp = bridge.dispatch_json(R"({"type":"hello"})");
    CHECK(response_has_error(resp, "unknown message type"));
}

// ── Value coercion helpers ───────────────────────────────────────────────

TEST_CASE("EditorBridge::get_float: handles missing key, type coercion, defaults",
          "[editor_bridge][issue-709]")
{
    auto obj = choc::value::createObject("");
    obj.addMember("as_int",   42);
    obj.addMember("as_int64", static_cast<int64_t>(7));
    obj.addMember("as_float", 1.5);
    obj.addMember("as_str",   std::string("nope"));

    CHECK(EditorBridge::get_float(obj, "as_int",   -1.0f) == Approx(42.0f));
    CHECK(EditorBridge::get_float(obj, "as_int64", -1.0f) == Approx(7.0f));
    CHECK(EditorBridge::get_float(obj, "as_float", -1.0f) == Approx(1.5f));
    CHECK(EditorBridge::get_float(obj, "as_str",   -1.0f) == Approx(-1.0f));   // wrong type → default
    CHECK(EditorBridge::get_float(obj, "absent",   42.5f) == Approx(42.5f));   // missing → default

    // Non-object value must fall back to default without throwing.
    auto arr = choc::value::createEmptyArray();
    CHECK(EditorBridge::get_float(arr, "any", 9.0f) == Approx(9.0f));
}

TEST_CASE("EditorBridge::get_uint: clamps negatives to zero, handles type coercion",
          "[editor_bridge][issue-709]")
{
    auto obj = choc::value::createObject("");
    obj.addMember("pos",   42);
    obj.addMember("neg",   -3);
    obj.addMember("flt",   2.7);
    obj.addMember("str",   std::string("nope"));

    CHECK(EditorBridge::get_uint(obj, "pos",   0) == 42u);
    CHECK(EditorBridge::get_uint(obj, "neg",   99) == 0u);    // negative clamped
    CHECK(EditorBridge::get_uint(obj, "flt",   0) == 2u);     // float truncated
    CHECK(EditorBridge::get_uint(obj, "str",   77) == 77u);   // wrong type → default
    CHECK(EditorBridge::get_uint(obj, "absent", 5) == 5u);
}

// Codex P2 on PR #711: floats above SIZE_MAX must clamp, not invoke
// undefined behavior via direct float→size_t cast.
TEST_CASE("EditorBridge::get_uint: clamps floats above SIZE_MAX to size_t max",
          "[editor_bridge][issue-709]")
{
    const double huge = 1e30;        // way above 2^64 ≈ 1.8e19
    const double huger = 1e300;      // exponent overflow territory
    auto obj = choc::value::createObject("");
    obj.addMember("huge_f64",  huge);
    obj.addMember("huger_f64", huger);
    obj.addMember("huge_f32",  static_cast<float>(1e30f));
    obj.addMember("neg_huge",  -1e20);

    constexpr auto kMax = std::numeric_limits<std::size_t>::max();
    CHECK(EditorBridge::get_uint(obj, "huge_f64",  0) == kMax);
    CHECK(EditorBridge::get_uint(obj, "huger_f64", 0) == kMax);
    CHECK(EditorBridge::get_uint(obj, "huge_f32",  0) == kMax);
    CHECK(EditorBridge::get_uint(obj, "neg_huge",  99) == 0u);  // negative still clamps to 0
}

TEST_CASE("EditorBridge::get_string: returns empty on missing or wrong-type",
          "[editor_bridge][issue-709]")
{
    auto obj = choc::value::createObject("");
    obj.addMember("name",   std::string("Spectr"));
    obj.addMember("count",  3);

    CHECK(EditorBridge::get_string(obj, "name")   == "Spectr");
    CHECK(EditorBridge::get_string(obj, "count")  == "");        // wrong type
    CHECK(EditorBridge::get_string(obj, "absent") == "");        // missing

    // Non-object input must return empty without throwing.
    auto arr = choc::value::createEmptyArray();
    CHECK(EditorBridge::get_string(arr, "any") == "");
}

// ── Response builder shapes ──────────────────────────────────────────────

TEST_CASE("EditorBridge::ok_response: emits canonical {ok:true} envelope",
          "[editor_bridge][issue-709]")
{
    const auto r = EditorBridge::ok_response();
    CHECK(response_ok(r));
    // Should not contain an "error" field when successful.
    CHECK(r.find("\"error\"") == std::string::npos);
}

TEST_CASE("EditorBridge::ok_response(extras): merges extra members into envelope",
          "[editor_bridge][issue-709]")
{
    auto extras = choc::value::createObject("");
    extras.addMember("name", std::string("Bridge Save"));
    extras.addMember("count", 7);

    const auto r = EditorBridge::ok_response(extras);
    CHECK(response_ok(r));
    CHECK(r.find("Bridge Save") != std::string::npos);
    CHECK(r.find("\"count\"") != std::string::npos);
}

TEST_CASE("EditorBridge::ok_response(non-object extras): falls back to bare ok envelope",
          "[editor_bridge][issue-709]")
{
    auto arr = choc::value::createEmptyArray();
    const auto r = EditorBridge::ok_response(arr);
    CHECK(response_ok(r));
    CHECK(r.find("\"error\"") == std::string::npos);
}

TEST_CASE("EditorBridge::err_response: emits {ok:false,error:...}",
          "[editor_bridge][issue-709]")
{
    const auto r = EditorBridge::err_response("custom failure");
    CHECK(response_has_error(r, "custom failure"));
}

// ── dispatch_webview_message — pre-split envelope path ───────────────────

TEST_CASE("EditorBridge::dispatch_webview_message: handles 'null' payload as empty object",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    bool called = false;
    bool was_object = false;
    bridge.add_handler("ping", [&](const choc::value::ValueView& payload) {
        called = true;
        was_object = payload.isObject();
        return EditorBridge::ok_response();
    });
    const auto r = bridge.dispatch_webview_message("ping", "null");
    CHECK(response_ok(r));
    CHECK(called);
    CHECK(was_object);
}

TEST_CASE("EditorBridge::dispatch_webview_message: parses payload_json",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    std::string seen;
    bridge.add_handler("name", [&](const choc::value::ValueView& payload) {
        seen = EditorBridge::get_string(payload, "v");
        return EditorBridge::ok_response();
    });
    const auto r = bridge.dispatch_webview_message("name", R"({"v":"hello"})");
    CHECK(response_ok(r));
    CHECK(seen == "hello");
}

TEST_CASE("EditorBridge::dispatch_webview_message: malformed payload_json errors",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    bridge.add_handler("anything", [](const auto&) { return EditorBridge::ok_response(); });
    const auto r = bridge.dispatch_webview_message("anything", "not json");
    CHECK(response_has_error(r, "malformed JSON"));
}

// ── Non-movable / non-copyable (Codex P1 on PR #711) ─────────────────────
//
// attach_webview installs a callback that captures a reference to the
// bridge. Allowing moves would let an attached bridge be relocated out
// from under that callback. EditorBridge enforces non-movability at
// the type level so the mistake surfaces at compile time, and these
// static_asserts lock that contract in.

static_assert(!std::is_copy_constructible_v<EditorBridge>,
              "EditorBridge must not be copyable");
static_assert(!std::is_copy_assignable_v<EditorBridge>,
              "EditorBridge must not be copy-assignable");
static_assert(!std::is_move_constructible_v<EditorBridge>,
              "EditorBridge must not be move-constructible");
static_assert(!std::is_move_assignable_v<EditorBridge>,
              "EditorBridge must not be move-assignable");

// ── Renderer attach helpers ──────────────────────────────────────────────
//
// attach_webview wires a WebViewPanel's set_message_handler through to
// the bridge's dispatch path. A minimal in-process WebViewPanel stub
// exercises that wiring without requiring a real WebView backend.

namespace {

class StubWebViewPanel : public pulp::view::WebViewPanel {
public:
    // Exposed so tests can drive the bridge-registered handler
    // (simulating JS `window.postMessage`).
    std::string deliver(const pulp::view::WebViewMessage& message) {
        REQUIRE(static_cast<bool>(handler_));
        return handler_(message);
    }

    // ── Pure virtuals from WebViewPanel ─────────────────────────────
    bool is_ready() const override { return true; }
    void set_ready_handler(ReadyHandler) override {}
    pulp::view::NativeViewHandle native_handle() override { return {}; }
    void navigate(const std::string&) override {}
    void set_html(const std::string&) override {}
    void evaluate_js(const std::string&) override {}
    void evaluate_js(const std::string&, EvalCallback) override {}
    void bind(const std::string&, JsCallback) override {}
    void set_message_handler(MessageHandler handler) override {
        handler_ = std::move(handler);
    }
    void post_message(const pulp::view::WebViewMessage&) override {}
    void set_size(uint32_t, uint32_t) override {}

private:
    MessageHandler handler_;
};

} // namespace

TEST_CASE("EditorBridge::attach_webview routes WebViewPanel messages through dispatch",
          "[editor_bridge][issue-709]")
{
    EditorBridge bridge;
    int called_with = 0;
    bridge.add_handler("set_value", [&](const auto& payload) {
        called_with = static_cast<int>(
            EditorBridge::get_float(payload, "value", 0.0f));
        return EditorBridge::ok_response();
    });

    StubWebViewPanel panel;
    bridge.attach_webview(panel);

    pulp::view::WebViewMessage msg;
    msg.type = "set_value";
    msg.payload_json = R"({"value":42})";
    const auto resp = panel.deliver(msg);
    CHECK(response_ok(resp));
    CHECK(called_with == 42);

    // Unknown type through the WebView path still yields the
    // framework-level unknown_type error.
    msg.type = "not_registered";
    msg.payload_json = "null";
    const auto resp2 = panel.deliver(msg);
    CHECK(response_has_error(resp2, "unknown message type"));
}

TEST_CASE("EditorBridge::attach_native_runtime is a no-op stub for #468",
          "[editor_bridge][issue-709][issue-468]")
{
    // The native-JS-runtime attach path is a declared seam for pulp
    // #468; the body is intentionally empty until JsRuntime exposes a
    // concrete postMessage primitive. This test locks in the stub's
    // no-throw, no-observable-effect behavior so a future wiring
    // change can't silently mutate the surface without updating here.
    EditorBridge bridge;
    bridge.add_handler("ping", [](const auto&) { return EditorBridge::ok_response(); });
    const auto count_before = bridge.handler_count();

    pulp::view::JsRuntime rt;
    bridge.attach_native_runtime(rt, "spectr");
    CHECK(bridge.handler_count() == count_before);

    // Dispatch still works after the stub attach — no state mutated.
    CHECK(response_ok(bridge.dispatch_json(R"({"type":"ping"})")));
}
