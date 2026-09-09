#pragma once

// Pulls in the BridgeRegistrars declarations so every registrar TU (which
// already includes this header) can define its `BridgeRegistrars::register_*`
// statics without a separate include.
#include "registrars.hpp"

#include <pulp/view/script_engine.hpp>

#include <pulp/runtime/trace.hpp>

#include <cstddef>
#include <string>
#include <type_traits>
#include <string_view>
#include <utility>

namespace pulp::view {

struct BridgeApiContext {
    ScriptEngine& engine;
};

// Every JS->C++ native is registered through this one call, so a span wrapped
// here attributes the native half of a script handler by function name with no
// per-call-site edit — which is the only way to see inside `dom_event_evaluate`
// for a script this repo does not own. It is compiled out entirely when tracing
// is off, so a shipping build registers the original callable with no added
// indirection.
template <typename Fn>
void register_bridge_function(BridgeApiContext& context, std::string_view name, Fn&& fn) {
#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED
    if constexpr (std::is_convertible_v<Fn&&, choc::javascript::Context::NativeFunction>) {
        choc::javascript::Context::NativeFunction inner(std::forward<Fn>(fn));
        std::string span = "js_native:" + std::string(name);
        context.engine.register_function(
            std::string(name),
            choc::javascript::Context::NativeFunction(
                [inner = std::move(inner), span = std::move(span)](
                    choc::javascript::ArgumentList args) {
                    PULP_TRACE_SCOPE_DYNAMIC("js", span);
                    return inner(args);
                }));
        return;
    } else if constexpr (std::is_convertible_v<Fn&&, NativeFunction>) {
        NativeFunction inner(std::forward<Fn>(fn));
        std::string span = "js_native:" + std::string(name);
        context.engine.register_function(
            std::string(name),
            NativeFunction([inner = std::move(inner), span = std::move(span)](
                               const choc::value::Value* args, size_t num_args) {
                PULP_TRACE_SCOPE_DYNAMIC("js", span);
                return inner(args, num_args);
            }));
        return;
    } else {
        context.engine.register_function(std::string(name), std::forward<Fn>(fn));
    }
#else
    context.engine.register_function(std::string(name), std::forward<Fn>(fn));
#endif
}

inline void register_bridge_host_object(BridgeApiContext& context,
                                        std::string_view name,
                                        HostObjectDescriptor descriptor) {
    context.engine.register_host_object(std::string(name), std::move(descriptor));
}

inline void register_bridge_promise_function(BridgeApiContext& context,
                                             std::string_view name,
                                             NativePromiseFunction fn) {
    context.engine.register_promise_function(std::string(name), std::move(fn));
}

} // namespace pulp::view
