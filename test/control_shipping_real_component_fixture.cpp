#include <pulp/inspect/capabilities.hpp>
#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_endpoint.hpp>
#include <pulp/inspect/control_service.hpp>

#include <optional>
#include <string_view>
#include <utility>

int main() {
    pulp::inspect::ControlBroker broker;
    pulp::inspect::ControlService service{broker};
    pulp::inspect::ControlEndpointConfig config;
    config.endpoint_path = "unused-control-shipping-fixture.sock";
    config.sdk_version = "fixture";
    config.broker_id = broker.broker_id().value;
    pulp::inspect::ControlEndpoint endpoint{
        service,
        [](std::string_view) -> std::optional<pulp::inspect::ControlConnectionAdmission> {
            return std::nullopt;
        },
        std::move(config)};
    return pulp::inspect::capability_id(
               pulp::inspect::InspectorCapability::SessionDescribe) == "session.describe"
               ? 0
               : 1;
}
