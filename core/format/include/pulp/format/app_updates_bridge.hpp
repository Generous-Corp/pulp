#pragma once

// EditorBridge messages that let a JS editor show and drive app updates.
//
// Register them once on the editor's bridge, in code every format shares:
//
//   pulp::format::add_app_update_handlers(editor_bridge_);
//
// A plug-in answers `available: false` (no service is installed there), so
// the editor hides its update controls; the standalone app answers with the
// live status. Messages (payload -> response, all `ok: true` plus the status
// object from app_update_status_json()):
//
//   pulp_updates_get            {}            -> status
//   pulp_updates_check          {}            -> status + started: bool
//   pulp_updates_set_automatic  {on: bool}    -> status + applied: bool
//   pulp_updates_open_releases  {}            -> status + opened: bool
//
// The JS side is `appUpdatesClient(dispatch)` in @pulp/react.

#include <pulp/view/editor_bridge.hpp>

namespace pulp::format {

inline constexpr const char* kAppUpdatesGetMessage = "pulp_updates_get";
inline constexpr const char* kAppUpdatesCheckMessage = "pulp_updates_check";
inline constexpr const char* kAppUpdatesSetAutomaticMessage = "pulp_updates_set_automatic";
inline constexpr const char* kAppUpdatesOpenReleasesMessage = "pulp_updates_open_releases";

void add_app_update_handlers(view::EditorBridge& bridge);

} // namespace pulp::format
