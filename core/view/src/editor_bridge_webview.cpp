#include "pulp/view/editor_bridge.hpp"

#include "pulp/view/web_view.hpp"

namespace pulp::view {

void EditorBridge::attach_webview(WebViewPanel& panel) {
    panel.set_message_handler([this](const WebViewMessage& message) {
        return dispatch_webview_message(message.type, message.payload_json);
    });
}

void EditorBridge::detach_webview(WebViewPanel& panel) {
    panel.set_message_handler({});
}

}  // namespace pulp::view
