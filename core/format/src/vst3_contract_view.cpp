#include <pulp/format/vst3_contract_view.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>

#include <pluginterfaces/base/keycodes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace pulp::format::vst3 {

using namespace Steinberg;

// What the host gives the editor: this view's port, a resize request through
// the host's IPlugFrame, and the adapter's host actions.
class ContractPlugView::Context final : public EditorContext {
  public:
    Context(ContractPlugView& view, ParamPort& port, EditorHostKind kind,
            HostActionHandler host_action)
        : view_(view), port_(port), kind_(kind), host_action_(std::move(host_action)) {}

    ParamPort& params() override {
        return port_;
    }
    VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const EditorSize& size) override {
        if (view_.plugFrame == nullptr || size.width == 0 || size.height == 0)
            return false;
        ViewRect rect(0, 0, static_cast<int32>(size.width), static_cast<int32>(size.height));
        return view_.plugFrame->resizeView(&view_, &rect) == kResultTrue;
    }
    bool host_action(const HostActionRequest& request) override {
        return host_action_ ? host_action_(request) : false;
    }
    EditorHostKind host_kind() const override {
        return kind_;
    }

  private:
    ContractPlugView& view_;
    ParamPort& port_;
    EditorHostKind kind_;
    HostActionHandler host_action_;
};

ContractPlugView* ContractPlugView::create(PulpVst3Processor& owner, Processor& processor,
                                           state::StateStore& store, EditorFactory& factory,
                                           HostActionHandler host_action) {
    if (!factory.supports(EditorRole::editor))
        return nullptr;
    auto* view = new ContractPlugView(owner, processor, store, std::move(host_action));
    view->editor_ = factory.create(processor, *view->context_, EditorRole::editor);
    if (!view->editor_) {
        runtime::log_error("VST3 editor: the editor factory built no editor");
        view->owner_ = nullptr;
        view->release();
        return nullptr;
    }
    const auto size = view->current_size();
    view->rect = ViewRect(0, 0, static_cast<int32>(size.width), static_cast<int32>(size.height));
    return view;
}

ContractPlugView::ContractPlugView(PulpVst3Processor& owner, Processor& processor,
                                   state::StateStore& store, HostActionHandler host_action)
    : CPluginView(nullptr), owner_(&owner), processor_(&processor),
      port_(std::make_unique<StateStoreParamPort>(store)),
      context_(std::make_unique<Context>(*this, *port_, processor.editor_host_kind(),
                                         std::move(host_action))) {}

void ContractPlugView::start_idle() {
    idle_.start(kIdleIntervalMs, [this] {
        if (editor_ && editor_attached_)
            editor_->idle();
    });
}

void ContractPlugView::stop_idle() {
    idle_.stop();
}

ContractPlugView::~ContractPlugView() {
    stop_idle();
    // A host that releases the view without removed() has already torn down
    // the window it was attached to.
    if (editor_ && editor_attached_)
        editor_->invalidate();
    editor_.reset();
    context_.reset();
    port_.reset();
    if (owner_ != nullptr)
        owner_->forget_contract_view(this);
}

void ContractPlugView::owner_terminated() {
    stop_idle();
    if (port_)
        port_->release_all();
    if (editor_) {
        // Only the plug-in is going: the host's window still holds the
        // editor's views, so take them out in order before destroying it.
        if (editor_attached_)
            editor_->detach();
        editor_.reset();
    }
    editor_attached_ = false;
    context_.reset();
    port_.reset();
    owner_ = nullptr;
    processor_ = nullptr;
}

NativeParentKind ContractPlugView::parent_kind(FIDString type) {
    if (type == nullptr)
        return NativeParentKind::none;
    if (std::strcmp(type, kPlatformTypeNSView) == 0)
        return NativeParentKind::cocoa_view;
    if (std::strcmp(type, kPlatformTypeUIView) == 0)
        return NativeParentKind::uikit_view;
    if (std::strcmp(type, kPlatformTypeHWND) == 0)
        return NativeParentKind::win32_window;
    if (std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0)
        return NativeParentKind::x11_window;
    return NativeParentKind::none;
}

tresult PLUGIN_API ContractPlugView::isPlatformTypeSupported(FIDString type) {
    const auto kind = parent_kind(type);
    if (!editor_ || kind == NativeParentKind::none)
        return kResultFalse;
    return editor_->supports_parent(kind) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ContractPlugView::attached(void* parent, FIDString type) {
    const auto kind = parent_kind(type);
    if (!editor_ || parent == nullptr || kind == NativeParentKind::none || editor_attached_)
        return kResultFalse;
    NativeParent native;
    native.kind = kind;
    if (kind == NativeParentKind::x11_window)
        native.window_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
    else
        native.handle = parent;
    if (!editor_->attach(native)) {
        runtime::log_error("VST3 editor: the editor did not attach to the host window");
        return kResultFalse;
    }
    editor_attached_ = true;
    start_idle();
    return CPluginView::attached(parent, type);
}

tresult PLUGIN_API ContractPlugView::removed() {
    stop_idle();
    // Closing the window mid-drag must still balance every beginEdit.
    if (port_)
        port_->release_all();
    if (editor_ && editor_attached_)
        editor_->detach();
    editor_attached_ = false;
    return CPluginView::removed();
}

EditorKeyEvent ContractPlugView::key_event(char16 key, int16 key_code, int16 modifiers,
                                           bool is_down) {
    EditorKeyEvent event;
    event.is_down = is_down;
    switch (key_code) {
    case KEY_SPACE:
        event.key = EditorKey::space;
        break;
    case KEY_RETURN:
    case KEY_ENTER:
        event.key = EditorKey::enter;
        break;
    case KEY_TAB:
        event.key = EditorKey::tab;
        break;
    case KEY_BACK:
        event.key = EditorKey::backspace;
        break;
    case KEY_ESCAPE:
        event.key = EditorKey::escape;
        break;
    case KEY_DELETE:
        event.key = EditorKey::delete_forward;
        break;
    case KEY_LEFT:
        event.key = EditorKey::left;
        break;
    case KEY_RIGHT:
        event.key = EditorKey::right;
        break;
    case KEY_UP:
        event.key = EditorKey::up;
        break;
    case KEY_DOWN:
        event.key = EditorKey::down;
        break;
    case KEY_HOME:
        event.key = EditorKey::home;
        break;
    case KEY_END:
        event.key = EditorKey::end;
        break;
    case KEY_PAGEUP:
        event.key = EditorKey::page_up;
        break;
    case KEY_PAGEDOWN:
        event.key = EditorKey::page_down;
        break;
    default:
        break;
    }
    if (key == static_cast<char16>(u' '))
        event.key = EditorKey::space;
    if (key != 0)
        event.character = static_cast<char32_t>(key);
    else if (event.key == EditorKey::space)
        event.character = U' ';

    std::uint8_t mods = 0;
    if ((modifiers & kShiftKey) != 0)
        mods |= editor_modifier_shift;
    if ((modifiers & kAlternateKey) != 0)
        mods |= editor_modifier_alt;
    // VST3's kCommandKey is Command on macOS and Ctrl on Windows and Linux;
    // kControlKey is the macOS Control key.
#if defined(__APPLE__)
    if ((modifiers & kCommandKey) != 0)
        mods |= editor_modifier_command;
#else
    if ((modifiers & kCommandKey) != 0)
        mods |= editor_modifier_control;
#endif
    if ((modifiers & kControlKey) != 0)
        mods |= editor_modifier_control;
    event.modifiers = mods;
    return event;
}

tresult ContractPlugView::offer(const EditorKeyEvent& event) {
    if (!editor_ || !editor_attached_)
        return kResultFalse;
    // kResultFalse hands the key back to the host: Space then reaches the
    // host's transport, which is what forward means.
    return editor_->offer_key(event) == KeyDisposition::consumed ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ContractPlugView::onKeyDown(char16 key, int16 keyCode, int16 modifiers) {
    return offer(key_event(key, keyCode, modifiers, true));
}

tresult PLUGIN_API ContractPlugView::onKeyUp(char16 key, int16 keyCode, int16 modifiers) {
    return offer(key_event(key, keyCode, modifiers, false));
}

EditorSize ContractPlugView::current_size() const {
    if (!editor_)
        return {};
    auto size = editor_->size();
    if (size.width == 0 || size.height == 0) {
        const auto hints = editor_->size_hints();
        size.width = hints.preferred_width;
        size.height = hints.preferred_height;
    }
    return size;
}

tresult PLUGIN_API ContractPlugView::getSize(ViewRect* size) {
    if (size == nullptr)
        return kInvalidArgument;
    if (!editor_)
        return kResultFalse;
    const auto s = current_size();
    *size = ViewRect(0, 0, static_cast<int32>(s.width), static_cast<int32>(s.height));
    return kResultTrue;
}

tresult PLUGIN_API ContractPlugView::onSize(ViewRect* newSize) {
    if (newSize == nullptr)
        return kInvalidArgument;
    if (!editor_ || newSize->getWidth() <= 0 || newSize->getHeight() <= 0)
        return kResultFalse;
    EditorSize size = editor_->size();
    size.width = static_cast<std::uint32_t>(newSize->getWidth());
    size.height = static_cast<std::uint32_t>(newSize->getHeight());
    if (!editor_->set_size(size))
        return kResultFalse;
    return CPluginView::onSize(newSize);
}

tresult PLUGIN_API ContractPlugView::setContentScaleFactor(ScaleFactor factor) {
    if (!editor_ || !(factor > 0.0f))
        return kResultFalse;
    // The editor keeps the scale in its size, so a later onSize() carries it.
    EditorSize size = current_size();
    size.scale = static_cast<double>(factor);
    return editor_->set_size(size) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ContractPlugView::onFocus(TBool state) {
    if (!editor_)
        return kResultFalse;
    editor_->focus_changed(state != 0);
    return kResultTrue;
}

tresult PLUGIN_API ContractPlugView::canResize() {
    if (!editor_)
        return kResultFalse;
    // Equal, non-zero minimum and maximum on both axes mean a fixed size.
    const auto h = editor_->size_hints();
    const bool fixed = h.min_width > 0 && h.min_width == h.max_width && h.min_height > 0 &&
                       h.min_height == h.max_height;
    return fixed ? kResultFalse : kResultTrue;
}

tresult PLUGIN_API ContractPlugView::checkSizeConstraint(ViewRect* rect) {
    if (rect == nullptr)
        return kInvalidArgument;
    if (!editor_ || rect->getWidth() <= 0 || rect->getHeight() <= 0)
        return kResultFalse;
    EditorSize proposed = editor_->size();
    proposed.width = static_cast<std::uint32_t>(rect->getWidth());
    proposed.height = static_cast<std::uint32_t>(rect->getHeight());
    auto adjusted = editor_->adjust_size(proposed);
    const auto h = editor_->size_hints();
    auto clamp = [](std::uint32_t v, std::uint32_t lo, std::uint32_t hi) {
        if (lo > 0 && v < lo)
            v = lo;
        if (hi > 0 && v > hi)
            v = hi;
        return v;
    };
    adjusted.width = clamp(adjusted.width, h.min_width, h.max_width);
    adjusted.height = clamp(adjusted.height, h.min_height, h.max_height);
    if (adjusted.width == 0 || adjusted.height == 0)
        return kResultFalse;
    rect->right = rect->left + static_cast<int32>(adjusted.width);
    rect->bottom = rect->top + static_cast<int32>(adjusted.height);
    return kResultTrue;
}

} // namespace pulp::format::vst3
