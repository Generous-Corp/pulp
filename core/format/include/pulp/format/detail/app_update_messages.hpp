#pragma once

// The app-update message responder over an already parsed payload, for a
// bridge that hands its handlers choc values (Pulp's EditorBridge does), so
// the payload is not serialised again only to be parsed back.

#include <choc/containers/choc_Value.h>

#include <string>
#include <string_view>

namespace pulp::format::detail {

/// app_update_message_response() (app_updates.hpp) for a parsed payload.
std::string app_update_message_response(std::string_view type,
                                        const choc::value::ValueView& payload);

} // namespace pulp::format::detail
