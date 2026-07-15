#include <pulp/events/message_loop_integration.hpp>

namespace pulp::events {

MainLoopPumpResult MessageLoopIntegration::pump_main_loop_for(
    std::chrono::milliseconds) {
    return MainLoopPumpResult::Unsupported;
}

} // namespace pulp::events
