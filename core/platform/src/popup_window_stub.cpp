// Platforms without popup windows from core/platform.

#include <pulp/platform/popup_window.hpp>

namespace pulp::platform {

struct PopupWindow::Impl {};

PopupWindow::PopupWindow(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
PopupWindow::~PopupWindow() = default;

std::unique_ptr<PopupWindow> PopupWindow::open(MainThreadScope&, const PopupWindowOptions&,
                                               std::function<void(PopupDismissReason)>) {
    return nullptr;
}

NativeViewHandle PopupWindow::content_view() const {
    return nullptr;
}
NativeWindowHandle PopupWindow::native_window() const {
    return nullptr;
}
void PopupWindow::set_size(float, float) {}
void PopupWindow::close() {}
bool PopupWindow::is_open() const {
    return false;
}
PopupRect PopupWindow::screen_frame() const {
    return {};
}
float PopupWindow::backing_scale() const {
    return 1.0f;
}

} // namespace pulp::platform
