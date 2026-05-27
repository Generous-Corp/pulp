#include <pulp/view/bubble_message.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

void BubbleMessageComponent::recompute_anchor() {
    if (has_explicit_anchor_) {
        position_ = compute_offset(explicit_anchor_);
        return;
    }
    if (!source_) return;
    Rect b = source_->bounds();
    Point anchor;
    switch (side_) {
        case Side::above:
            anchor = {b.x + b.width * 0.5f, b.y};
            break;
        case Side::below:
            anchor = {b.x + b.width * 0.5f, b.y + b.height};
            break;
        case Side::left:
            anchor = {b.x, b.y + b.height * 0.5f};
            break;
        case Side::right:
            anchor = {b.x + b.width, b.y + b.height * 0.5f};
            break;
    }
    position_ = compute_offset(anchor);
}

}  // namespace pulp::view
