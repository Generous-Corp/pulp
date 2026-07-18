#pragma once

// Pulls in the BridgeRegistrars declarations so every registrar TU (which
// already includes this header) can define its `BridgeRegistrars::register_*`
// statics without a separate include.
#include "registrars.hpp"

#include <pulp/view/script_engine.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace pulp::view {

struct BridgeApiContext {
    ScriptEngine& engine;
};

template <typename Fn>
void register_bridge_function(BridgeApiContext& context, std::string_view name, Fn&& fn) {
    context.engine.register_function(std::string(name), std::forward<Fn>(fn));
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
