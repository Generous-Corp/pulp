#include <pulp/view/press_reach.hpp>

#include <pulp/view/overlay_dismissal.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp> // ScrollView

#include <cmath>
#include <cstdio>

namespace pulp::view {
namespace {

bool is_self_or_descendant(const View* needle, const View* ancestor) {
    for (const View* v = needle; v != nullptr; v = v->parent())
        if (v == ancestor)
            return true;
    return false;
}

// Interactive in the sense that matters here: a press could land on it. A
// hidden, disabled or pointer-events:none control is authored state, not a
// wiring defect, so it is counted and skipped rather than reported.
bool interactive(const View& view) {
    return view.visible() && view.enabled() && view.hit_testable() &&
           view.pointer_events() != View::PointerEvents::none;
}

// Every ancestor must also admit the press, or the control is unreachable for
// a reason that belongs to the ancestor. That IS a finding, so this is only
// used to classify the SKIP buckets, never to excuse a failure.
bool ancestors_interactive(const View& view, const View& root) {
    for (const View* v = view.parent(); v != nullptr; v = v->parent()) {
        if (!v->visible())
            return false;
        if (v->pointer_events() == View::PointerEvents::none)
            return false;
        if (v == &root)
            break;
    }
    return true;
}

bool advertises(const View& view, PressChannel channel) {
    return channel == PressChannel::click ? static_cast<bool>(view.on_click)
                                          : static_cast<bool>(view.on_context_menu);
}

std::string name_of(const View* view) {
    if (view == nullptr)
        return "(nothing)";
    if (!view->id().empty())
        return view->id();
    int depth = 0;
    for (const View* v = view->parent(); v != nullptr; v = v->parent()) {
        ++depth;
        if (!v->id().empty())
            return "(anon +" + std::to_string(depth) + " under " + v->id() + ")";
    }
    return "(anon, no named ancestor)";
}

void walk(View& view, View& root, const PressReachOptions& options, PressReachAudit& audit);

} // namespace

const char* press_channel_name(PressChannel channel) {
    return channel == PressChannel::click ? "click" : "context-menu";
}

PressReach reach_of_press(View& root, Point root_pt, PressChannel channel) {
    PressReach out;
    out.hit = root.hit_test(root_pt);
    if (out.hit == nullptr)
        return out;

    // Both channels bubble from the hit view to the root, first listener
    // wins. That is `dispatch_context_menu`'s walk for the right button and
    // `deliver_mouse_down` / `simulate_click`'s walk for the left one; this
    // function must never invent a third rule, because its whole value is
    // answering what the REAL dispatch would do.
    for (View* v = out.hit; v != nullptr; v = v->parent()) {
        const bool carries = channel == PressChannel::click ? static_cast<bool>(v->on_click)
                                                            : static_cast<bool>(v->on_context_menu);
        if (carries) {
            out.handler = v;
            out.local = point_to_local(root_pt, v, &root);
            break;
        }
        if (v == &root)
            break;
    }
    return out;
}

std::optional<Rect> rect_in_root(const View& view, View& root) {
    float x = 0.0f;
    float y = 0.0f;
    bool saw_root = false;
    // Stop AT the root: `root_pt` is root-local, so the root's own bounds
    // offset is not part of the sum, and anything above the root belongs to a
    // different coordinate space entirely.
    for (const View* node = &view; node != nullptr; node = node->parent()) {
        if (node == &root) {
            saw_root = true;
            break;
        }
        x += node->bounds().x;
        y += node->bounds().y;
        // `ScrollView::hit_test` descends with `child + scroll - bounds`, so
        // the outbound direction subtracts what the inbound one added. Summing
        // bounds alone once reported a scrolled panel's rows below an 860-tall
        // root, where `hit_test` correctly answers "nothing is here" -- and
        // that reads exactly like the defect this instrument hunts.
        if (const auto* scroll = dynamic_cast<const ScrollView*>(node->parent())) {
            x -= scroll->scroll_x();
            y -= scroll->scroll_y();
        }
    }
    if (!saw_root)
        return std::nullopt; // not in this tree

    const auto box = view.local_bounds();
    const Rect candidate{x, y, box.width, box.height};

    // Verify rather than trust. A `set_scale` or transform matrix anywhere on
    // the path makes the additive walk wrong, and a wrong rect would make this
    // instrument a generator of confident nonsense. `point_to_local` is the
    // function the hosts route real presses through, so round-tripping through
    // it is an exact check, not a heuristic.
    if (candidate.width > 0.0f && candidate.height > 0.0f) {
        const Point centre{candidate.x + candidate.width * 0.5f,
                           candidate.y + candidate.height * 0.5f};
        const Point local = point_to_local(centre, const_cast<View*>(&view), &root);
        if (!view.local_bounds().contains(local))
            return std::nullopt;
    }
    return candidate;
}

namespace {

void probe(View& view, View& root, PressChannel channel, const PressReachOptions& options,
           PressReachAudit& audit) {
    if (!advertises(view, channel))
        return;

    if (!interactive(view) || !ancestors_interactive(view, root)) {
        ++audit.skipped_not_interactive;
        return;
    }

    const auto painted = rect_in_root(view, root);
    if (!painted) {
        ++audit.skipped_indeterminate;
        return;
    }

    if (painted->width < options.min_dimension || painted->height < options.min_dimension) {
        return;
    }

    // A zero-area rect is a finding on its own terms, and has to be judged
    // BEFORE probing: `Rect::contains` is half-open, so such a rect holds no
    // point at all and there is no honest centre to press. Whatever `hit_test`
    // returns at that coordinate belongs to some other view.
    if (painted->width <= 0.0f || painted->height <= 0.0f) {
        ++audit.examined;
        UnreachablePressTarget finding;
        finding.view = &view;
        finding.id = name_of(&view);
        finding.painted = *painted;
        finding.channel = channel;
        finding.probe = Point{painted->x, painted->y};
        finding.reason = "paints a zero-area box (" + std::to_string(painted->width) + "x" +
                         std::to_string(painted->height) +
                         "), and Rect::contains is half-open, so no press can land in it";
        audit.unreachable.push_back(std::move(finding));
        return;
    }

    const Point centre{painted->x + painted->width * 0.5f, painted->y + painted->height * 0.5f};

    const auto root_box = root.local_bounds();
    if (!root_box.contains(centre)) {
        // Scrolled away or positioned outside the window. Genuinely
        // unreachable right now, but by layout rather than by wiring, and
        // reporting it would drown the wiring defects.
        ++audit.skipped_offscreen;
        return;
    }

    ++audit.examined;
    const auto reach = reach_of_press(root, centre, channel);

    if (!is_self_or_descendant(reach.hit, &view)) {
        UnreachablePressTarget finding;
        finding.view = &view;
        finding.id = name_of(&view);
        finding.painted = *painted;
        finding.channel = channel;
        finding.probe = centre;
        finding.reached = reach.hit;
        finding.reached_id = name_of(reach.hit);
        finding.reason = "a press at the centre of the rect it paints resolves "
                         "to " +
                         finding.reached_id + ", which is outside its subtree";
        audit.unreachable.push_back(std::move(finding));
        return;
    }

    if (reach.handler == nullptr) {
        UnreachablePressTarget finding;
        finding.view = &view;
        finding.id = name_of(&view);
        finding.painted = *painted;
        finding.channel = channel;
        finding.probe = centre;
        finding.reached = reach.hit;
        finding.reached_id = name_of(reach.hit);
        finding.reason = std::string("the press lands inside its subtree, on ") +
                         finding.reached_id + ", but no " + press_channel_name(channel) +
                         " handler resolves there";
        audit.unreachable.push_back(std::move(finding));
    }
}

void walk(View& view, View& root, const PressReachOptions& options, PressReachAudit& audit) {
    if (options.audit_click)
        probe(view, root, PressChannel::click, options, audit);
    if (options.audit_context_menu)
        probe(view, root, PressChannel::context_menu, options, audit);
    for (std::size_t i = 0; i < view.child_count(); ++i)
        if (auto* child = view.child_at(i))
            walk(*child, root, options, audit);
}

} // namespace

PressReachAudit audit_press_reach(View& root, const PressReachOptions& options) {
    PressReachAudit audit;
    walk(root, root, options, audit);
    return audit;
}

std::string format_press_reach_audit(const PressReachAudit& audit) {
    std::string out;
    char line[512];

    for (const auto& f : audit.unreachable) {
        std::snprintf(line, sizeof(line), "UNREACHABLE %-13s %s paints (%.1f,%.1f %.1fx%.1f); %s\n",
                      press_channel_name(f.channel), f.id.c_str(), f.painted.x, f.painted.y,
                      f.painted.width, f.painted.height, f.reason.c_str());
        out += line;
    }

    // Always printed, findings or not. "0 unreachable" over 0 examined is a
    // blind run, and a reader who only sees the finding list cannot tell the
    // two apart.
    std::snprintf(line, sizeof(line),
                  "census: %d press target(s) examined; %d skipped "
                  "(not interactive), %d skipped (offscreen), %d skipped "
                  "(rect indeterminate); %zu unreachable\n",
                  audit.examined, audit.skipped_not_interactive, audit.skipped_offscreen,
                  audit.skipped_indeterminate, audit.unreachable.size());
    out += line;
    return out;
}

bool View::simulate_context_click(Point root_pos) {
    return route_context_press(*this, root_pos).handled;
}

} // namespace pulp::view
