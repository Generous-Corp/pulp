// AU v2 Cocoa view factory for a processor that supplies an editor factory
// (`Processor::editor_factory()`, see pulp/format/editor_host.hpp).
//
// pulp_add_plugin compiles this into the *_AU module of a plug-in that
// declares `UI <package>`. Such a plug-in links no Pulp view layer (UI implies
// NO_EDITOR), so this file names no view, canvas or render type: the editor
// is the UI package's, opened through a ContractEditorSession.
//
// The host finds the factory through kAudioUnitProperty_CocoaUI and asks it
// for a view. The factory returns a container NSView that owns the session;
// the editor is attached into that container at once, so the container is the
// editor's NativeParent for its whole life. The mapping:
//
//   uiViewForAudioUnit:withSize:   -> a session (factory create) attached to
//                                     the container, sized to the editor
//   container -setFrameSize:       -> Editor::set_size() (the host resized it)
//   container backing scale change -> Editor::set_size() with the new scale
//   container moves into / out of a window -> Editor::visibility_changed()
//   key events reaching the container -> Editor::offer_key(); a forwarded key
//                                     continues up the responder chain to the
//                                     host, so Space stays its transport key
//   container deallocates          -> every gesture released, Editor::detach()
//   AU instance disposed first     -> its ContractSessionRegistry closes the
//                                     editor in order (gestures released,
//                                     Editor::detach(), destroyed); the
//                                     container is still alive, so the editor
//                                     is never invalidated here. Disposed off
//                                     the main thread, the gestures are
//                                     released and the store cut at once and
//                                     the close is posted to the main thread,
//                                     never waited for. Cutting the store does
//                                     wait for a parameter call already running
//                                     on the main thread, so a host that
//                                     disposes off the main thread while holding
//                                     a lock that call needs would hang (AU's
//                                     threading model already rules that out);
//                                     such a wait is logged after a second
//
// Idle runs from the session's own main-thread timer. Edits go through the session's StateStoreParamPort
// into the AU instance's store, which the adapter already reports to the host.
//
// Class names are per plug-in (PULP_AU_CONTRACT_VIEW_FACTORY_CLASS and
// PULP_AU_CONTRACT_HOST_VIEW_CLASS, set by pulp_add_plugin): Objective-C class
// names are process-global, so two Pulp AUs in one host must not share them.

#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <Cocoa/Cocoa.h>

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/detail/appkit_contract_key.hpp>
#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>

#include <cstdint>
#include <memory>
#include <utility>

#ifndef PULP_AU_CONTRACT_VIEW_FACTORY_CLASS
#define PULP_AU_CONTRACT_VIEW_FACTORY_CLASS PulpAUContractViewFactory
#endif
#ifndef PULP_AU_CONTRACT_HOST_VIEW_CLASS
#define PULP_AU_CONTRACT_HOST_VIEW_CLASS PulpAUContractHostView
#endif
#define PULP_AU_CONTRACT_STRINGIFY_IMPL(x) #x
#define PULP_AU_CONTRACT_STRINGIFY(x) PULP_AU_CONTRACT_STRINGIFY_IMPL(x)

namespace pulp::format::au {

/// The contract's key event for an AppKit key event.
EditorKeyEvent contract_key_event(unsigned short key_code, NSString* characters,
                                  NSEventModifierFlags flags, bool is_down, bool is_repeat) {
    return detail::appkit_contract_key_event(key_code, characters, flags, is_down, is_repeat);
}

} // namespace pulp::format::au

// The container the host embeds. It owns the session; the editor draws into it.
@interface PULP_AU_CONTRACT_HOST_VIEW_CLASS : NSView {
    std::shared_ptr<pulp::format::ContractEditorSession> _session;
    bool _applyingEditorSize;
}
- (void)adoptSession:(std::shared_ptr<pulp::format::ContractEditorSession>)session;
- (pulp::format::ContractEditorSession*)session;
- (void)applyEditorSize;
@end

@implementation PULP_AU_CONTRACT_HOST_VIEW_CLASS

- (void)adoptSession:(std::shared_ptr<pulp::format::ContractEditorSession>)session {
    _session = std::move(session);
}

- (pulp::format::ContractEditorSession*)session {
    return _session.get();
}

- (BOOL)isFlipped {
    return YES;
}

// Size the container to the editor without echoing the change back to it.
- (void)applyEditorSize {
    if (!_session || _session->editor() == nullptr)
        return;
    const auto size = _session->size();
    _applyingEditorSize = true;
    [super setFrameSize:NSMakeSize(size.width, size.height)];
    _applyingEditorSize = false;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (_applyingEditorSize || !_session || newSize.width < 1.0 || newSize.height < 1.0)
        return;
    _session->set_size(static_cast<std::uint32_t>(newSize.width),
                       static_cast<std::uint32_t>(newSize.height));
}

- (void)viewWillMoveToWindow:(NSWindow*)window {
    [super viewWillMoveToWindow:window];
    if (_session)
        _session->visibility_changed(window != nil);
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_session && self.window != nil)
        _session->set_scale(self.window.backingScaleFactor);
}

- (bool)offerKey:(NSEvent*)event down:(bool)down {
    if (!_session)
        return false;
    const auto key =
        pulp::format::au::contract_key_event(event.keyCode, event.charactersIgnoringModifiers,
                                             event.modifierFlags, down, down && event.isARepeat);
    return _session->offer_key(key) == pulp::format::KeyDisposition::consumed;
}

- (void)keyDown:(NSEvent*)event {
    if (![self offerKey:event down:true])
        [super keyDown:event]; // up the responder chain to the host
}

- (void)keyUp:(NSEvent*)event {
    if (![self offerKey:event down:false])
        [super keyUp:event];
}

- (void)dealloc {
    // The host dropped the view: an orderly close, so the container is still
    // a valid parent while the editor detaches.
    if (_session) {
        _session->detach();
        _session.reset();
    }
    [super dealloc];
}

@end

@interface PULP_AU_CONTRACT_VIEW_FACTORY_CLASS : NSObject <AUCocoaUIBase>
@end

@implementation PULP_AU_CONTRACT_VIEW_FACTORY_CLASS

- (unsigned)interfaceVersion {
    return 0;
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)inAU withSize:(NSSize)inPreferredSize {
    using namespace pulp;
    (void)inPreferredSize; // the editor states its own size

    format::au::PulpEditorContext ctx{};
    UInt32 size = sizeof(ctx);
    const OSStatus status = AudioUnitGetProperty(inAU, format::au::kPulpEditorContextProperty,
                                                 kAudioUnitScope_Global, 0, &ctx, &size);
    if (status != noErr || ctx.processor == nullptr || ctx.store == nullptr) {
        runtime::log_error("AU v2 editor: could not fetch editor context (status={})",
                           static_cast<int>(status));
        return nil;
    }
    auto* factory = ctx.processor->editor_factory();
    if (factory == nullptr)
        return nil;
    if (format::detail::editor_launch_blocked_by_environment()) {
        runtime::log_info("AU v2 editor: disabled in headless/CI/test environment");
        return nil;
    }

    PULP_AU_CONTRACT_HOST_VIEW_CLASS* view =
        [[PULP_AU_CONTRACT_HOST_VIEW_CLASS alloc] initWithFrame:NSZeroRect];
    format::ContractEditorSession::HostHooks hooks;
    // The view owns the session, so it outlives every hook call.
    hooks.request_resize = [view](const format::EditorSize& requested) {
        // AU v2 has no resize request: the host follows the view's frame.
        [view setFrameSize:NSMakeSize(requested.width, requested.height)];
        return true;
    };
    auto session = format::ContractEditorSession::create(*ctx.processor, *ctx.store, *factory,
                                                         std::move(hooks));
    if (!session) {
        [view release];
        return nil;
    }
    // Shared: the instance's registry may hold the session past this view
    // while it closes the editor on the main thread.
    [view adoptSession:std::shared_ptr<format::ContractEditorSession>(std::move(session))];
    [view applyEditorSize];
    format::NativeParent parent;
    parent.kind = format::NativeParentKind::cocoa_view;
    parent.handle = (__bridge void*)view;
    if (![view session]->attach(parent)) {
        [view release];
        return nil;
    }
    if (ctx.contract_sessions != nullptr)
        [view session]->register_with(*ctx.contract_sessions);
    return [view autorelease];
}

@end

namespace pulp::format::au {

ContractEditorSession* contract_view_session(void* ns_view) {
    id view = (__bridge id)ns_view;
    if (view == nil || ![view isKindOfClass:[PULP_AU_CONTRACT_HOST_VIEW_CLASS class]])
        return nullptr;
    auto* session = [(PULP_AU_CONTRACT_HOST_VIEW_CLASS*)view session];
    return session != nullptr && session->editor() != nullptr ? session : nullptr;
}

namespace {

constexpr const char* kContractViewFactoryClassName =
    PULP_AU_CONTRACT_STRINGIFY(PULP_AU_CONTRACT_VIEW_FACTORY_CLASS);

bool fill_contract_cocoa_view_info(void* outData) {
    @autoreleasepool {
        if (outData == nullptr)
            return false;
        auto* info = static_cast<AudioUnitCocoaViewInfo*>(outData);
        Class factory_class = [PULP_AU_CONTRACT_VIEW_FACTORY_CLASS class];
        NSBundle* bundle = factory_class ? [NSBundle bundleForClass:factory_class] : nil;
        if (bundle == nil)
            return false;
        // -[NSBundle bundleURL], not CFBundleCopyBundleURL: the CFBundle path
        // traps under pointer authentication in sandboxed hosts (see
        // au_v2_cocoa_view.mm).
        @try {
            NSURL* url = [bundle bundleURL];
            if (url == nil)
                return false;
            CFStringRef name = CFStringCreateWithCString(
                kCFAllocatorDefault, kContractViewFactoryClassName, kCFStringEncodingUTF8);
            if (name == nullptr)
                return false;
            // The host releases both (+1 retained).
            info->mCocoaAUViewBundleLocation = (CFURLRef)CFBridgingRetain(url);
            info->mCocoaAUViewClass[0] = name;
            return true;
        } @catch (NSException*) {
            return false;
        }
    }
}

// Advertise the factory through the adapters' kAudioUnitProperty_CocoaUI hook.
struct ContractCocoaViewInfoRegistration {
    ContractCocoaViewInfoRegistration() {
        g_cocoa_view_info_filler = &fill_contract_cocoa_view_info;
    }
    ~ContractCocoaViewInfoRegistration() {
        if (g_cocoa_view_info_filler == &fill_contract_cocoa_view_info)
            g_cocoa_view_info_filler = nullptr;
    }
};
ContractCocoaViewInfoRegistration g_register_contract_cocoa_view_info;

} // namespace

} // namespace pulp::format::au
