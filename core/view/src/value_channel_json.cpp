#include <pulp/view/value_channel_json.hpp>

namespace pulp::view {

const char* value_channel_shape_name(ValueChannelShape shape) noexcept {
    switch (shape) {
        case ValueChannelShape::scalar: return "scalar";
        case ValueChannelShape::meter:  return "meter";
        case ValueChannelShape::vector: return "vector";
        case ValueChannelShape::events: return "events";
    }
    return "scalar";
}

choc::value::Value value_channel_to_value(const ValueChannelInfo& info) {
    auto o = choc::value::createObject("ValueChannel");
    o.addMember("name", info.name);
    o.addMember("unit", info.unit);
    o.addMember("shape", value_channel_shape_name(info.shape));
    o.addMember("neutral", static_cast<double>(info.neutral));
    return o;
}

choc::value::Value value_channels_to_value(const ValueChannelSet* channels) {
    return value_channels_to_value(
        channels ? std::span<const ValueChannelInfo>(channels->infos())
                 : std::span<const ValueChannelInfo>{});
}

choc::value::Value
value_channels_to_value(std::span<const ValueChannelInfo> channels) {
    auto arr = choc::value::createEmptyArray();
    for (const auto& info : channels) arr.addArrayElement(value_channel_to_value(info));
    return arr;
}

} // namespace pulp::view
