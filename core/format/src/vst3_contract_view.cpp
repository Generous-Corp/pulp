#include <pulp/format/vst3_contract_view.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/state/store.hpp>

#include <pluginterfaces/base/keycodes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace pulp::format::vst3 {

using namespace Steinberg;

ContractPlugView* ContractPlugView::create(PulpVst3Processor& owner, Processor& processor,
                                           state::StateStore& store, EditorFactory& factory,
                                           HostActionHandler host_action) {
    auto* view = new ContractPlugView(owner);
    ContractEditorSession::HostHooks hooks;
    hooks.request_resize = [view](const EditorSize& size) {
        if (view->plugFrame == nullptr)
            return false;
        ViewRect rect(0, 0, static_cast<int32>(size.width), static_cast<int32>(size.height));
        return view->plugFrame->resizeView(view, &rect) == kResultTrue;
    };
    hooks.host_action = std::move(host_action);
    view->session_ = ContractEditorSession::create(processor, store, factory, std::move(hooks));
    if (!view->session_) {
        view->owner_ = nullptr;
        view->release();
        return nullptr;
    }
    const auto size = view->session_->size();
    view->rect = ViewRect(0, 0, static_cast<int32>(size.width), static_cast<int32>(size.height));
    return view;
}

ContractPlugView::ContractPlugView(PulpVst3Processor& owner)
    : CPluginView(nullptr), owner_(&owner) {}

ContractPlugView::~ContractPlugView() {
    // Destroying the session invalidates an editor the host never detached.
    session_.reset();
    if (owner_ != nullptr)
        owner_->forget_contract_view(this);
}

void ContractPlugView::owner_terminated() {
    // The host still holds this view, so its NSView is alive: close the editor
    // in order rather than invalidating it.
    if (session_)
        session_->close_for_owner();
    owner_ = nullptr;
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
    if (editor() == nullptr || kind == NativeParentKind::none)
        return kResultFalse;
    return editor()->supports_parent(kind) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ContractPlugView::attached(void* parent, FIDString type) {
    const auto kind = parent_kind(type);
    if (!session_ || parent == nullptr || kind == NativeParentKind::none)
        return kResultFalse;
    NativeParent native;
    native.kind = kind;
    if (kind == NativeParentKind::x11_window)
        native.window_id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent));
    else
        native.handle = parent;
    if (!session_->attach(native))
        return kResultFalse;
    return CPluginView::attached(parent, type);
}

tresult PLUGIN_API ContractPlugView::removed() {
    if (session_)
        session_->detach();
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
    // kResultFalse hands the key back to the host: Space then reaches the
    // host's transport, which is what forward means.
    if (!session_)
        return kResultFalse;
    return session_->offer_key(event) == KeyDisposition::consumed ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ContractPlugView::onKeyDown(char16 key, int16 keyCode, int16 modifiers) {
    return offer(key_event(key, keyCode, modifiers, true));
}

tresult PLUGIN_API ContractPlugView::onKeyUp(char16 key, int16 keyCode, int16 modifiers) {
    return offer(key_event(key, keyCode, modifiers, false));
}

tresult PLUGIN_API ContractPlugView::getSize(ViewRect* size) {
    if (size == nullptr)
        return kInvalidArgument;
    if (editor() == nullptr)
        return kResultFalse;
    const auto s = session_->size();
    *size = ViewRect(0, 0, static_cast<int32>(s.width), static_cast<int32>(s.height));
    return kResultTrue;
}

tresult PLUGIN_API ContractPlugView::onSize(ViewRect* newSize) {
    if (newSize == nullptr)
        return kInvalidArgument;
    if (!session_ || newSize->getWidth() <= 0 || newSize->getHeight() <= 0)
        return kResultFalse;
    if (!session_->set_size(static_cast<std::uint32_t>(newSize->getWidth()),
                            static_cast<std::uint32_t>(newSize->getHeight())))
        return kResultFalse;
    return CPluginView::onSize(newSize);
}

tresult PLUGIN_API ContractPlugView::setContentScaleFactor(ScaleFactor factor) {
    if (!session_)
        return kResultFalse;
    return session_->set_scale(static_cast<double>(factor)) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ContractPlugView::onFocus(TBool state) {
    if (editor() == nullptr)
        return kResultFalse;
    session_->focus_changed(state != 0);
    return kResultTrue;
}

tresult PLUGIN_API ContractPlugView::canResize() {
    return session_ && session_->resizable() ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API ContractPlugView::checkSizeConstraint(ViewRect* rect) {
    if (rect == nullptr)
        return kInvalidArgument;
    if (!session_ || rect->getWidth() <= 0 || rect->getHeight() <= 0)
        return kResultFalse;
    auto width = static_cast<std::uint32_t>(rect->getWidth());
    auto height = static_cast<std::uint32_t>(rect->getHeight());
    if (!session_->constrain(width, height))
        return kResultFalse;
    rect->right = rect->left + static_cast<int32>(width);
    rect->bottom = rect->top + static_cast<int32>(height);
    return kResultTrue;
}

} // namespace pulp::format::vst3
