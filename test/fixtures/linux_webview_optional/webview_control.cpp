#include <pulp/view/web_view.hpp>

int main() {
    auto webview = pulp::view::WebViewPanel::create();
    return webview ? 0 : 1;
}
