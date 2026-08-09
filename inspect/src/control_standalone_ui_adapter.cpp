#include <pulp/inspect/control_standalone_ui_adapter.hpp>

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulp::inspect {
namespace {

view::View* find_unique_node(view::View& root, std::string_view id) {
    view::View* match = nullptr;
    bool duplicate = false;
    const auto visit = [&](const auto& self, view::View& node) -> void {
        if (node.id() == id) {
            if (match)
                duplicate = true;
            else
                match = &node;
        }
        for (std::size_t index = 0; index < node.child_count(); ++index)
            self(self, *node.child_at(index));
    };
    visit(visit, root);
    return duplicate ? nullptr : match;
}

std::optional<view::KeyCode> key_code(std::string_view key) {
    if (key.size() == 1) {
        char value = key.front();
        if (value >= 'A' && value <= 'Z')
            value = static_cast<char>(value + ('a' - 'A'));
        if ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
            value == ' ' || value == ';' || value == '\'')
            return static_cast<view::KeyCode>(value);
    }
    struct NamedKey {
        std::string_view name;
        view::KeyCode code;
    };
    static constexpr NamedKey names[]{
        {"Left", view::KeyCode::left},       {"Right", view::KeyCode::right},
        {"Up", view::KeyCode::up},           {"Down", view::KeyCode::down},
        {"Home", view::KeyCode::home},       {"End", view::KeyCode::end_},
        {"PageUp", view::KeyCode::page_up},  {"PageDown", view::KeyCode::page_down},
        {"Backspace", view::KeyCode::backspace}, {"Delete", view::KeyCode::delete_},
        {"Tab", view::KeyCode::tab},         {"Enter", view::KeyCode::enter},
        {"Escape", view::KeyCode::escape},   {"Space", view::KeyCode::space},
        {"F1", view::KeyCode::f1},           {"F2", view::KeyCode::f2},
        {"F3", view::KeyCode::f3},           {"F4", view::KeyCode::f4},
        {"F5", view::KeyCode::f5},           {"F6", view::KeyCode::f6},
        {"F7", view::KeyCode::f7},           {"F8", view::KeyCode::f8},
        {"F9", view::KeyCode::f9},           {"F10", view::KeyCode::f10},
        {"F11", view::KeyCode::f11},         {"F12", view::KeyCode::f12},
    };
    const auto found = std::ranges::find(names, key, &NamedKey::name);
    return found == std::end(names) ? std::nullopt : std::optional{found->code};
}

view::MouseButton mouse_button(std::uint8_t button) {
    switch (button) {
    case 0:
    case 1: return view::MouseButton::left;
    case 2: return view::MouseButton::right;
    case 3: return view::MouseButton::middle;
    default: return view::MouseButton::none;
    }
}

} // namespace

class ControlStandaloneUiAdapter::Impl {
  public:
    explicit Impl(ControlStandaloneUiAdapterConfig config)
        : root(config.root), window(config.window), instance_id(std::move(config.instance_id)),
          instance_generation(std::move(config.instance_generation)),
          view_generation(std::move(config.view_generation)), scale(config.capture_scale) {}

    bool exact(const ControlUiExactTarget& target) const {
        return target.instance_id == instance_id &&
               target.instance_generation == instance_generation &&
               target.view_generation == view_generation;
    }

    view::View& root;
    view::WindowHost& window;
    std::string instance_id;
    std::string instance_generation;
    std::string view_generation;
    float scale = 1.0f;
    view::ViewCapture pointer_target;
    std::optional<ControlUiAuthorityOwner> pointer_owner;
    view::Point pointer_position{};
    view::MouseButton pointer_button = view::MouseButton::none;
    view::ViewCapture focus_target;
    std::optional<ControlUiAuthorityOwner> focus_owner;
};

ControlStandaloneUiAdapter::ControlStandaloneUiAdapter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ControlStandaloneUiAdapter::~ControlStandaloneUiAdapter() = default;

std::shared_ptr<ControlStandaloneUiAdapter>
ControlStandaloneUiAdapter::create(ControlStandaloneUiAdapterConfig config) {
    if (config.instance_id.empty() || config.instance_generation.empty() ||
        config.view_generation.empty() ||
        config.instance_id.size() > kControlUiMaximumTargetBytes ||
        config.instance_generation.size() > kControlUiMaximumTargetBytes ||
        config.view_generation.size() > kControlUiMaximumViewGenerationBytes ||
        !std::isfinite(config.capture_scale) || config.capture_scale <= 0.0f ||
        config.capture_scale > 8.0f)
        return {};
    return std::shared_ptr<ControlStandaloneUiAdapter>(
        new ControlStandaloneUiAdapter(std::make_unique<Impl>(std::move(config))));
}

InspectorCapture ControlStandaloneUiAdapter::capture_png() {
    const auto size = impl_->window.get_content_size();
    auto png = impl_->window.capture_back_buffer_png();
    if (size.width == 0 || size.height == 0 || png.empty() ||
        !view::passes_capture_content_floor(png))
        return {.error = "standalone window capture failed", .error_code = "capture_failed"};
    return {.png = std::move(png), .width = size.width, .height = size.height};
}

InspectorCapture
ControlStandaloneUiAdapter::capture_node_png(const ControlUiExactTarget& target) {
    if (!impl_->exact(target))
        return {.error = "stale UI generation", .error_code = "session_stale"};
    auto* node = find_unique_node(impl_->root, target.node_id);
    if (!node)
        return {.error = "exact UI node unavailable", .error_code = "target_unavailable"};
    view::ViewCapture captured;
    captured.set(node);
    node = captured.live_in(impl_->root);
    if (!node)
        return {.error = "exact UI node became stale", .error_code = "session_stale"};
    const auto bounds = node->local_bounds();
    if (!std::isfinite(bounds.width) || !std::isfinite(bounds.height) || bounds.width <= 0.0f ||
        bounds.height <= 0.0f || bounds.width > 1'048'576.0f || bounds.height > 1'048'576.0f)
        return {.error = "exact UI node has invalid bounds", .error_code = "capture_failed"};
    const auto width = static_cast<std::uint32_t>(std::ceil(bounds.width));
    const auto height = static_cast<std::uint32_t>(std::ceil(bounds.height));
    constexpr std::uint64_t kMaximumCapturePixels = 16u * 1024u * 1024u;
    const auto scaled_width = static_cast<std::uint64_t>(std::ceil(width * impl_->scale));
    const auto scaled_height = static_cast<std::uint64_t>(std::ceil(height * impl_->scale));
    if (scaled_width == 0 || scaled_height == 0 || scaled_width > 16'384 ||
        scaled_height > 16'384 || scaled_width > kMaximumCapturePixels / scaled_height)
        return {.error = "exact UI node exceeds capture pixel budget",
                .error_code = "capture_too_large"};
    auto result = view::capture_view(*node, width, height, impl_->scale);
    if (!result.ok)
        return {.png = std::move(result.png),
                .error = std::move(result.reason),
                .error_code = "capture_failed"};
    return {.png = std::move(result.png),
            .width = static_cast<std::uint32_t>(scaled_width),
            .height = static_cast<std::uint32_t>(scaled_height)};
}

ControlUiApplyStatus
ControlStandaloneUiAdapter::dispatch_input(const ControlUiExactTarget& target,
                                           const ControlUiAuthorityOwner& owner,
                                           const ControlUiInput& input) {
    if (owner.client_id.empty() || owner.grant_id.empty() || owner.client_principal.empty())
        return ControlUiApplyStatus::InvalidEvent;
    if (!impl_->exact(target))
        return ControlUiApplyStatus::StaleGeneration;
    auto* node = find_unique_node(impl_->root, target.node_id);
    if (!node)
        return ControlUiApplyStatus::TargetUnavailable;
    view::ViewCapture live;
    live.set(node);
    node = live.live_in(impl_->root);
    if (!node)
        return ControlUiApplyStatus::StaleGeneration;

    if (const auto* pointer = std::get_if<ControlUiPointerInput>(&input)) {
        const view::Point point{static_cast<float>(pointer->x), static_cast<float>(pointer->y)};
        const auto button = mouse_button(pointer->button);
        if (button == view::MouseButton::none)
            return ControlUiApplyStatus::InvalidEvent;
        if (pointer->phase == ControlUiPointerInput::Phase::Down) {
            if (impl_->pointer_target.has_value() || impl_->pointer_owner ||
                impl_->root.hit_test(point) != node)
                return ControlUiApplyStatus::InvalidEvent;
            const bool remained_live =
                view::deliver_mouse_down(impl_->root, node, point, 0, 1, true, button);
            if (!remained_live) {
                impl_->window.mark_dirty();
                return ControlUiApplyStatus::Applied;
            }
            impl_->pointer_target.set(node);
            impl_->pointer_owner = owner;
            impl_->pointer_button = button;
        } else {
            auto* retained = impl_->pointer_target.live_in(impl_->root);
            if (!retained || retained != node || impl_->pointer_owner != owner ||
                button != impl_->pointer_button)
                return ControlUiApplyStatus::InvalidEvent;
            if (pointer->phase == ControlUiPointerInput::Phase::Move)
                view::deliver_mouse_drag(impl_->root, retained, point, 0, 1,
                                         view::PointerType::mouse, 0.5f, button);
            else {
                view::deliver_mouse_up(
                    impl_->root, retained, point, 0, 1,
                    {.fire_click = [](const std::function<void()>& click, const std::string&,
                                      std::uint16_t) {
                         if (click)
                             click();
                     }},
                    button);
                impl_->pointer_target.reset();
                impl_->pointer_owner.reset();
                impl_->pointer_button = view::MouseButton::none;
            }
        }
        impl_->pointer_position = point;
        impl_->window.mark_dirty();
        return ControlUiApplyStatus::Applied;
    }
    if (const auto* keyboard = std::get_if<ControlUiKeyboardInput>(&input)) {
        const auto code = key_code(keyboard->key);
        if (!code || impl_->focus_owner != owner ||
            view::focused_input_under_root(impl_->root) != node)
            return ControlUiApplyStatus::InvalidEvent;
        node->on_key_event({.key = *code,
                            .modifiers = 0,
                            .is_down = keyboard->phase == ControlUiKeyboardInput::Phase::Down,
                            .is_repeat = keyboard->repeat});
    } else if (const auto* focus = std::get_if<ControlUiFocusInput>(&input)) {
        if (focus->focused) {
            if ((impl_->focus_owner && impl_->focus_owner != owner) || !node->focusable() ||
                !view::transfer_input_focus(impl_->root, node) ||
                view::focused_input_under_root(impl_->root) != node)
                return ControlUiApplyStatus::TargetUnavailable;
            impl_->focus_target.set(node);
            impl_->focus_owner = owner;
        } else if (impl_->focus_owner == owner &&
                   impl_->focus_target.live_in(impl_->root) == node &&
                   view::focused_input_under_root(impl_->root) == node) {
            view::transfer_input_focus(impl_->root, nullptr);
            impl_->focus_target.reset();
            impl_->focus_owner.reset();
        } else {
            return ControlUiApplyStatus::InvalidEvent;
        }
    } else if (const auto* text = std::get_if<ControlUiTextInput>(&input)) {
        if (!node->accepts_text_input() || impl_->focus_owner != owner ||
            impl_->focus_target.live_in(impl_->root) != node ||
            view::focused_input_under_root(impl_->root) != node)
            return ControlUiApplyStatus::InvalidEvent;
        node->on_text_input({text->text});
    }
    impl_->window.mark_dirty();
    return ControlUiApplyStatus::Applied;
}

void ControlStandaloneUiAdapter::release_controller(
    const std::optional<ControlUiAuthorityOwner>& owner) noexcept {
    if (!impl_)
        return;
    if (!owner || impl_->pointer_owner == owner) {
        if (auto* target = impl_->pointer_target.live_in(impl_->root))
            view::deliver_mouse_cancel(impl_->root, target, impl_->pointer_position, 0, 1,
                                       impl_->pointer_button);
        impl_->pointer_target.reset();
        impl_->pointer_owner.reset();
        impl_->pointer_button = view::MouseButton::none;
    }
    if (!owner || impl_->focus_owner == owner) {
        if (auto* target = impl_->focus_target.live_in(impl_->root);
            target && view::focused_input_under_root(impl_->root) == target)
            view::transfer_input_focus(impl_->root, nullptr);
        impl_->focus_target.reset();
        impl_->focus_owner.reset();
    }
}

} // namespace pulp::inspect
