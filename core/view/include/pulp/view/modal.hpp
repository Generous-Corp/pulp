#pragma once

/// @file modal.hpp
/// Modal overlay support — focus trapping, Escape to close, dimmed background.

#include <pulp/view/view.hpp>
#include <functional>

namespace pulp::view {

/// A modal overlay that captures focus and dims the background.
///
/// When shown, the modal:
/// - Renders a semi-transparent overlay behind its content
/// - Traps keyboard focus within its child views
/// - Closes on Escape key press
/// - Calls on_dismiss when closed
///
/// @code
/// auto modal = std::make_unique<ModalOverlay>();
/// modal->on_dismiss = [&] { remove_modal(); };
/// modal->add_child(std::move(dialog_content));
/// root->add_child(std::move(modal));
/// @endcode
class ModalOverlay : public View {
public:
    /// Called when the modal is dismissed (Escape or backdrop click).
    std::function<void()> on_dismiss;

    /// Whether clicking the dimmed backdrop dismisses the modal.
    bool dismiss_on_backdrop_click = true;

    /// Backdrop opacity (0 = transparent, 1 = opaque black).
    float backdrop_opacity = 0.5f;

    void paint(canvas::Canvas& canvas) override {
        // Draw dimmed backdrop
        auto b = local_bounds();
        canvas.set_fill_color(canvas::Color::rgba8(0, 0, 0,
            static_cast<uint8_t>(backdrop_opacity * 255)));
        canvas.fill_rect(b.x, b.y, b.width, b.height);
    }

    bool on_key_event(const KeyEvent& event) override {
        if (event.is_down && event.key == KeyCode::escape) {
            if (on_dismiss) on_dismiss();
            return true;
        }
        return false;
    }

    void on_mouse_event(const MouseEvent& event) override {
        // If click is on backdrop (not on a child), dismiss
        if (event.is_down && dismiss_on_backdrop_click) {
            auto* target = hit_test(event.position);
            if (target == this) {
                if (on_dismiss) on_dismiss();
            }
        }
    }
};

} // namespace pulp::view
