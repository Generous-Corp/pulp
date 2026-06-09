// widget_bridge/state_binding_api.cpp - parameter state registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <cstddef>
#include <string>

namespace pulp::view {

void WidgetBridge::register_state_binding_api() {
    BridgeApiContext api{engine_};

    // getParam(name) -> get parameter value from store (normalized)
    register_bridge_function(api, "getParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                return choc::value::createFloat64(store_.get_normalized(info->id));
            }
        }
        return choc::value::createFloat64(0);
    });

    // setParam(name, normalized_value) -> set parameter in store
    register_bridge_function(api, "setParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                store_.set_normalized(info->id, static_cast<float>(value));
                break;
            }
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view
