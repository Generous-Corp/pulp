#include <pulp/view/widget_bridge.hpp>

namespace pulp::view {

void WidgetBridge::set_value_channels(ValueChannelSet* channels) {
    set_value_channel_access(
        [channels](const ValueChannelVisitor& visitor) { visitor(channels); });
}

void WidgetBridge::set_value_channel_access(ValueChannelAccess access) {
    value_channel_access_ = std::move(access);
}

void WidgetBridge::visit_value_channels(
    const ValueChannelVisitor& visitor) const {
    if (value_channel_access_) value_channel_access_(visitor);
    else visitor(nullptr);
}

}  // namespace pulp::view
