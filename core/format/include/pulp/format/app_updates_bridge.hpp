#pragma once

// Registers the app-update editor messages (app_updates.hpp) on Pulp's
// EditorBridge, in code every format shares:
//
//   pulp::format::add_app_update_handlers(editor_bridge_);
//
// The messages and their responses are app_update_message_response()'s; a
// bridge other than Pulp's registers them by calling that function directly.

#include <pulp/format/app_updates.hpp>
#include <pulp/view/editor_bridge.hpp>

namespace pulp::format {

void add_app_update_handlers(view::EditorBridge& bridge);

} // namespace pulp::format
