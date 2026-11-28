#include "pulp/view/editor_bridge_attach.hpp"

#include "pulp/view/web_view.hpp"

namespace pulp::view {

void attach_webview(EditorBridge& bridge, WebViewPanel& panel) {
    panel.set_message_handler([&bridge](const WebViewMessage& message) {
        return bridge.dispatch_webview_message(message.type, message.payload_json);
    });
}

void detach_webview(EditorBridge& /*bridge*/, WebViewPanel& panel) {
    panel.set_message_handler({});
}

} // namespace pulp::view
