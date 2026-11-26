#include <pulp/vellum_components/inspector_overlay.hpp>

#include <vellum/view/input_events.hpp>

#include <utility>

namespace pulp::vellum_components {

namespace {

constexpr float kNudgeStep = 1.0f;
constexpr float kNudgeStepShift = 10.0f;
constexpr float kNudgeStepMain = 100.0f;

} // namespace

bool InspectorOverlay::emit_edit(const View& target, std::string_view path, EditValue value,
                                 std::string_view source) const {
    const auto& anchor = target.anchor_id();
    if (anchor.empty() || !edit_sink_)
        return false;
    edit_sink_(anchor, path, value, source);
    return true;
}

void InspectorOverlay::start_field_edit(View& target, std::string field_path, float initial_value) {
    field_target_ = &target;
    field_edit_.begin(std::move(field_path), initial_value);
}

void InspectorOverlay::clear_field_edit() {
    field_edit_.clear();
    field_target_ = nullptr;
}

bool InspectorOverlay::begin_field_edit(std::string field_path, float initial_value) {
    if (!selected_ || field_path.empty())
        return false;
    start_field_edit(*selected_, std::move(field_path), initial_value);
    return true;
}

void InspectorOverlay::apply_edit_buffer_to_view() {
    if (!field_target_)
        return;
    // Each keystroke previews live; an empty or non-numeric buffer leaves the
    // last previewed value in place.
    if (const auto value = field_edit_.value())
        overlay::write_field_value(*field_target_, field_edit_.path(), *value);
}

bool InspectorOverlay::commit_field_edit() {
    if (!field_edit_.active())
        return false;
    View* target = field_target_;
    const std::string path = field_edit_.path();
    const auto value = field_edit_.value();
    clear_field_edit();
    if (!value || !target || !overlay::tree_contains(root_, target))
        return false;
    // The last keystroke may not have been a digit, so write the final value.
    overlay::write_field_value(*target, path, *value);
    return emit_edit(*target, path, EditValue{*value}, "inspector-keyboard-edit");
}

void InspectorOverlay::cancel_field_edit() {
    if (!field_edit_.active())
        return;
    if (field_target_ && overlay::tree_contains(root_, field_target_))
        overlay::write_field_value(*field_target_, field_edit_.path(), field_edit_.original());
    clear_field_edit();
}

bool InspectorOverlay::handle_edit_key(const ::vellum::view::KeyEvent& event) {
    using ::vellum::view::KeyCode;
    // The target may have left the tree since the last paint.
    if (!field_target_ || !overlay::tree_contains(root_, field_target_)) {
        clear_field_edit();
        return false;
    }

    switch (event.key) {
    case KeyCode::escape:
        cancel_field_edit();
        return true;
    case KeyCode::enter:
        commit_field_edit();
        return true;
    case KeyCode::tab: {
        View* target = field_target_;
        const std::string current = field_edit_.path();
        commit_field_edit();
        const auto& paths = overlay::kEditableFieldPaths;
        const std::size_t count = std::size(paths);
        for (std::size_t i = 0; i < count; ++i) {
            if (paths[i] != current)
                continue;
            const std::size_t next =
                event.isShiftDown() ? (i + count - 1) % count : (i + 1) % count;
            // The previous commit may have changed what the next field reads.
            start_field_edit(*target, std::string(paths[next]),
                             overlay::read_field_value(*target, paths[next]));
            break;
        }
        return true;
    }
    case KeyCode::backspace:
        field_edit_.backspace();
        apply_edit_buffer_to_view();
        return true;
    case KeyCode::up:
    case KeyCode::down: {
        float step = kNudgeStep;
        if (event.isShiftDown())
            step = kNudgeStepShift;
        if (event.isMainModifier())
            step = kNudgeStepMain;
        if (event.key == KeyCode::down)
            step = -step;
        field_edit_.nudge(step);
        apply_edit_buffer_to_view();
        return true;
    }
    case KeyCode::left:
        field_edit_.move_left();
        return true;
    case KeyCode::right:
        field_edit_.move_right();
        return true;
    case KeyCode::home:
        field_edit_.move_home();
        return true;
    case KeyCode::end_:
        field_edit_.move_end();
        return true;
    default:
        break;
    }

    // A chord is not a digit; leave it for the shortcut it belongs to.
    if (event.isCmdDown() || event.isCtrlDown())
        return false;
    const int code = static_cast<int>(event.key);
    if (code >= '0' && code <= '9' && field_edit_.insert_digit(static_cast<char>(code))) {
        apply_edit_buffer_to_view();
        return true;
    }
    return false;
}

} // namespace pulp::vellum_components
