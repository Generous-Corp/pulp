// Platforms without native asynchronous dialogs, alerts or a URL opener:
// every backend reports that it cannot show anything, and the common code
// delivers the empty or "unavailable" result: posted on platforms with a
// main-thread backend, inline on those without one.

#include "dialog_internal.hpp"

namespace pulp::platform::detail {

std::function<void()> present_native_alert(NativeViewHandle, const AlertRequest&,
                                           const AlertButtonPlan&, std::function<void(long)>) {
    return {};
}

std::function<void()> present_native_open(NativeViewHandle, const FileDialog::OpenRequest&,
                                          std::function<void(std::vector<std::string>)>) {
    return {};
}

std::function<void()>
present_native_save(NativeViewHandle, const FileDialog::SaveRequest&, std::optional<std::string>,
                    std::function<void(std::optional<std::string>, std::string)>) {
    return {};
}

std::function<void()> present_native_menu(MainThreadScope&, const MenuAnchor&, MenuModel,
                                          MenuPlacement, std::function<void(std::optional<int>)>,
                                          std::shared_ptr<void>&) {
    return {};
}

bool native_open_url(const std::string&) {
    return false;
}

std::vector<std::string> native_content_type_ids(const std::vector<FileFilter>&) {
    return {};
}

} // namespace pulp::platform::detail
