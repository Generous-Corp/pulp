// Asynchronous anchored menus: placement arithmetic, the presenter seam and
// one-shot delivery. The NSMenu itself lives in platform/mac/popup_menu_mac.mm.

#include <pulp/platform/popup_menu.hpp>

#include "dialog_internal.hpp"

#include <mutex>
#include <utility>

namespace pulp::platform {

namespace {

std::mutex g_menu_presenter_mutex;
MenuPresenter g_menu_presenter;

MenuPresenter current_menu_presenter() {
    std::lock_guard<std::mutex> lock(g_menu_presenter_mutex);
    return g_menu_presenter;
}

} // namespace

MenuPoint menu_location_in_view(const MenuAnchor& anchor, MenuPlacement placement,
                                bool view_is_flipped, float view_height) {
    // Anchors are top-left based; AppKit views are bottom-left based unless
    // flipped.
    const float top = anchor.y;
    const float bottom = anchor.y + anchor.height;
    const float edge = placement == MenuPlacement::below_anchor ? bottom : top;
    return {anchor.x, view_is_flipped ? edge : view_height - edge};
}

void set_menu_presenter(MenuPresenter presenter) {
    std::lock_guard<std::mutex> lock(g_menu_presenter_mutex);
    g_menu_presenter = std::move(presenter);
}

DialogSession show_menu_async(MainThreadScope& scope, const MenuAnchor& anchor, MenuModel model,
                              MenuPlacement placement,
                              std::function<void(std::optional<int>)> done) {
    using Result = std::optional<int>;
    auto state = std::make_shared<detail::DialogSessionState>();
    auto finish = detail::make_finisher<Result>(scope, state, std::move(done));

    const MenuPresenter presenter = current_menu_presenter();
    if (presenter.present) {
        presenter.present(anchor, model, placement, [finish](Result id) { finish(id); });
        auto cancel = presenter.cancel;
        state->close = [cancel, finish] {
            if (cancel)
                cancel();
            finish(std::nullopt);
        };
    } else {
        state->close = detail::present_native_menu(
            scope, anchor, std::move(model), placement, [finish](Result id) { finish(id); },
            state->resources);
    }
    if (!state->close) {
        finish(std::nullopt);
        return DialogSession(state);
    }
    detail::bind_to_scope(scope, state);
    return DialogSession(state);
}

} // namespace pulp::platform
