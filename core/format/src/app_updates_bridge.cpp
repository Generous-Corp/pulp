#include <pulp/format/app_updates.hpp>
#include <pulp/format/app_updates_bridge.hpp>
#include <pulp/format/detail/app_update_messages.hpp>

#include <string_view>

namespace pulp::format {

void add_app_update_handlers(view::EditorBridge& bridge) {
    for (const char* type : kAppUpdatesMessages) {
        const std::string_view name = type;
        bridge.add_handler(type, [name](const choc::value::ValueView& payload) {
            return detail::app_update_message_response(name, payload);
        });
    }
}

} // namespace pulp::format
