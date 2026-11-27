#pragma once

// The starter bridge source `pulp import-design --from claude` writes next to
// the generated JS view. Defined in pulp::view-tooling; the view-free
// EditorBridge core does not provide it.

#include <string>

namespace pulp::view {

/// Build the starter `bridge_handlers.cpp` text. Pure string builder, no I/O.
/// The output registers add_handler() examples on a pulp::view::EditorBridge
/// and attaches it to a WebViewPanel with attach_webview().
std::string render_claude_bridge_scaffold(const std::string& generated_js_path);

} // namespace pulp::view
