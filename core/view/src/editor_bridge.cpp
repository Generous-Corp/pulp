#include "pulp/view/editor_bridge.hpp"

#include "pulp/view/web_view.hpp"

#include <choc/text/choc_JSON.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::view {

struct EditorBridge::Impl {
    std::unordered_map<std::string, Handler> handlers;
};

EditorBridge::EditorBridge() : impl_(std::make_unique<Impl>()) {}
EditorBridge::~EditorBridge() = default;
// Move ops are deleted in the header — see comment there.

void EditorBridge::add_handler(std::string_view type, Handler fn) {
    impl_->handlers[std::string(type)] = std::move(fn);
}

void EditorBridge::remove_handler(std::string_view type) {
    impl_->handlers.erase(std::string(type));
}

bool EditorBridge::has_handler(std::string_view type) const noexcept {
    return impl_->handlers.find(std::string(type)) != impl_->handlers.end();
}

std::size_t EditorBridge::handler_count() const noexcept {
    return impl_->handlers.size();
}

std::string EditorBridge::dispatch(std::string_view type,
                                   const choc::value::ValueView& payload) const noexcept
{
    try {
        const auto it = impl_->handlers.find(std::string(type));
        if (it == impl_->handlers.end()) {
            return err_response("unknown message type");
        }
        return it->second(payload);
    } catch (...) {
        return err_response("internal error");
    }
}

std::string EditorBridge::dispatch_json(std::string_view json) const noexcept {
    choc::value::Value root;
    try {
        root = choc::json::parse(json);
    } catch (...) {
        return err_response("malformed JSON");
    }
    if (!root.isObject()) {
        return err_response("envelope must be an object");
    }

    // Read the type field.
    if (!root.hasObjectMember("type")) {
        return err_response("envelope missing 'type'");
    }
    const auto type_v = root["type"];
    if (!type_v.isString()) {
        return err_response("envelope missing 'type'");
    }
    const std::string type{type_v.getString()};
    if (type.empty()) {
        return err_response("envelope missing 'type'");
    }

    // Payload is optional; pass an empty object when absent.
    if (!root.hasObjectMember("payload")) {
        auto empty = choc::value::createObject("");
        return dispatch(type, empty);
    }
    return dispatch(type, root["payload"]);
}

std::string EditorBridge::dispatch_webview_message(std::string_view type,
                                                   std::string_view payload_json) const noexcept
{
    // Per WebViewMessage contract, payload_json is "null" when no
    // payload was supplied — treat that the same as an empty object so
    // handlers don't see a non-object ValueView.
    if (payload_json.empty() || payload_json == "null") {
        auto empty = choc::value::createObject("");
        return dispatch(type, empty);
    }
    choc::value::Value payload;
    try {
        payload = choc::json::parse(payload_json);
    } catch (...) {
        return err_response("malformed JSON");
    }
    return dispatch(type, payload);
}

void EditorBridge::attach_webview(WebViewPanel& panel) {
    panel.set_message_handler([this](const WebViewMessage& m) {
        return dispatch_webview_message(m.type, m.payload_json);
    });
}

void EditorBridge::detach_webview(WebViewPanel& panel) {
    panel.set_message_handler({});
}

void EditorBridge::attach_native_runtime(JsRuntime& /*runtime*/,
                                         std::string_view /*handler_name*/) {
    // Stub for pulp #468 (Claude Design import lane). The full wiring
    // lands when JsRuntime exposes a postMessage-equivalent primitive
    // that calls back into C++. The interface is defined here so #468
    // plugs in without designing a parallel dispatch model.
}

// ── Static helpers ──────────────────────────────────────────────────────

float EditorBridge::get_float(const choc::value::ValueView& v,
                              const char* key,
                              float dflt) noexcept
{
    try {
        if (!v.isObject() || !v.hasObjectMember(key)) return dflt;
        const auto e = v[key];
        if (e.isFloat64()) return static_cast<float>(e.getFloat64());
        if (e.isFloat32()) return e.getFloat32();
        if (e.isInt64())   return static_cast<float>(e.getInt64());
        if (e.isInt32())   return static_cast<float>(e.getInt32());
        return dflt;
    } catch (...) {
        return dflt;
    }
}

std::size_t EditorBridge::get_uint(const choc::value::ValueView& v,
                                   const char* key,
                                   std::size_t dflt) noexcept
{
    // Clamp BOTH ends before casting. C++ float-to-int conversion is
    // undefined behavior when the source value doesn't fit the target
    // type, and a malformed editor payload can easily exceed SIZE_MAX
    // (or even overflow the float's exponent). Codex flagged this on
    // PR #711.
    constexpr auto kMaxAsDouble = static_cast<double>(
        std::numeric_limits<std::size_t>::max());
    constexpr auto kMaxAsInt64 = std::numeric_limits<std::size_t>::max() >
                                  static_cast<std::size_t>(
                                      std::numeric_limits<int64_t>::max())
                                      ? std::numeric_limits<int64_t>::max()
                                      : static_cast<int64_t>(
                                          std::numeric_limits<std::size_t>::max());

    auto clamp_double = [](double x) -> std::size_t {
        if (x < 0.0) return std::size_t{0};
        if (!(x < kMaxAsDouble)) return std::numeric_limits<std::size_t>::max();
        return static_cast<std::size_t>(x);
    };

    try {
        if (!v.isObject() || !v.hasObjectMember(key)) return dflt;
        const auto e = v[key];
        if (e.isInt32()) {
            const auto x = e.getInt32();
            return x < 0 ? std::size_t{0} : static_cast<std::size_t>(x);
        }
        if (e.isInt64()) {
            const auto x = e.getInt64();
            if (x < 0) return std::size_t{0};
            // On 32-bit size_t, an int64 may exceed SIZE_MAX.
            if (x > kMaxAsInt64) return std::numeric_limits<std::size_t>::max();
            return static_cast<std::size_t>(x);
        }
        if (e.isFloat64()) return clamp_double(e.getFloat64());
        if (e.isFloat32()) return clamp_double(static_cast<double>(e.getFloat32()));
        return dflt;
    } catch (...) {
        return dflt;
    }
}

std::string EditorBridge::get_string(const choc::value::ValueView& v,
                                     const char* key) noexcept
{
    try {
        if (!v.isObject() || !v.hasObjectMember(key)) return {};
        const auto e = v[key];
        if (!e.isString()) return {};
        return std::string(e.getString());
    } catch (...) {
        return {};
    }
}

std::string EditorBridge::ok_response() noexcept {
    try {
        auto obj = choc::value::createObject("");
        obj.addMember("ok", true);
        return choc::json::toString(obj, /*useLineBreaks=*/false);
    } catch (...) {
        // Fall back to a hand-written envelope rather than throw.
        return std::string{R"({"ok":true})"};
    }
}

std::string EditorBridge::ok_response(const choc::value::ValueView& extras) noexcept {
    try {
        if (!extras.isObject()) {
            return ok_response();
        }
        auto obj = choc::value::createObject("");
        obj.addMember("ok", true);
        const auto n = extras.size();
        for (uint32_t i = 0; i < n; ++i) {
            const auto name = extras.getObjectMemberAt(i).name;
            obj.addMember(std::string(name), extras[name]);
        }
        return choc::json::toString(obj, /*useLineBreaks=*/false);
    } catch (...) {
        return ok_response();
    }
}

std::string EditorBridge::err_response(std::string_view msg) noexcept {
    try {
        auto obj = choc::value::createObject("");
        obj.addMember("ok",    false);
        obj.addMember("error", std::string(msg));
        return choc::json::toString(obj, /*useLineBreaks=*/false);
    } catch (...) {
        return std::string{R"({"ok":false,"error":"err_response failed"})"};
    }
}

} // namespace pulp::view
