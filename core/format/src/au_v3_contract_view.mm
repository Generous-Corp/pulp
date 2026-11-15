// The AU v3 contract view; see au_v3_contract_view.h.
//
// Compiled into the AU v3 framework (macOS) and the AU v3 extension (iOS)
// beside the view controllers, and names no view, canvas or render type: the
// editor is the UI package's, opened through a ContractEditorSession.

#include "au_v3_contract_view.h"

#include <pulp/format/detail/main_thread_timer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>

#if TARGET_OS_OSX
#include <pulp/format/detail/appkit_contract_key.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

@implementation PULP_AUV3_CONTRACT_VIEW_CLASS {
    std::shared_ptr<pulp::format::ContractEditorSession> _session;
    PulpAUv3ContractResizeRequest _resizeRequest;
    // True while the editor is moving this view's frame, so the change is not
    // echoed back to it as a host resize.
    BOOL _applyingEditorSize;
    // The last scale handed to the editor, so a scale change is noticed on
    // layout (UIKit's trait callbacks are deprecated for this).
    CGFloat _pushedScale;
}

+ (instancetype)openViewForProcessor:(pulp::format::Processor*)processor
                               store:(pulp::state::StateStore*)store
                            registry:(pulp::format::ContractSessionRegistry*)registry
                       resizeRequest:(PulpAUv3ContractResizeRequest)resizeRequest {
    if (processor == nullptr || store == nullptr)
        return nil;
    auto* factory = processor->editor_factory();
    if (factory == nullptr)
        return nil;

    PULP_AUV3_CONTRACT_VIEW_CLASS* view = [[self alloc] initWithFrame:CGRectZero];
    view->_resizeRequest = [resizeRequest copy];

    pulp::format::ContractEditorSession::HostHooks hooks;
    // The view owns the session, so it outlives every hook call.
    hooks.request_resize = [view](const pulp::format::EditorSize& requested) {
        return [view requestEditorSize:CGSizeMake(requested.width, requested.height)];
    };
    auto session =
        pulp::format::ContractEditorSession::create(*processor, *store, *factory, std::move(hooks));
    if (!session) {
        [view release];
        return nil;
    }
    // Shared: the audio unit's registry may hold the session past this view
    // while it closes the editor on the main thread.
    view->_session = std::shared_ptr<pulp::format::ContractEditorSession>(std::move(session));
    [view applyEditorSize];

    pulp::format::NativeParent parent;
#if TARGET_OS_OSX
    parent.kind = pulp::format::NativeParentKind::cocoa_view;
#else
    parent.kind = pulp::format::NativeParentKind::uikit_view;
#endif
    parent.handle = (__bridge void*)view;
    if (!view->_session->attach(parent)) {
        pulp::runtime::log_error("AU v3 editor: the editor did not attach to its view");
        [view closeEditor];
        [view release];
        return nil;
    }
    // The editor may report its size only once attached.
    [view applyEditorSize];
    if (registry != nullptr)
        view->_session->register_with(*registry);
    return [view autorelease];
}

- (pulp::format::ContractEditorSession*)session {
    return _session.get();
}

- (void)closeEditor {
    if (_session) {
        _session->detach();
        _session.reset();
    }
    [_resizeRequest release];
    _resizeRequest = nil;
}

- (void)dealloc {
    [self closeEditor];
    [super dealloc];
}

#pragma mark Sizing

// Move this view to the editor's size without echoing the change back to it.
- (void)applyEditorSize {
    if (!_session || _session->editor() == nullptr)
        return;
    const auto size = _session->size();
    [self setFrameWithoutEcho:CGSizeMake(size.width, size.height)];
}

- (void)setFrameWithoutEcho:(CGSize)size {
    const BOOL previous = _applyingEditorSize;
    _applyingEditorSize = YES;
#if TARGET_OS_OSX
    [super setFrameSize:NSSizeFromCGSize(size)];
#else
    CGRect frame = self.frame;
    frame.size = size;
    self.frame = frame;
#endif
    _applyingEditorSize = previous;
}

// The editor asks the host to resize. AU v3 has no accept or refuse callback:
// the controller publishes preferredContentSize and resizes its own view, and
// the host follows. Refused (false) when the controller has none to ask.
- (BOOL)requestEditorSize:(CGSize)size {
    if (!pulp::format::detail::is_main_thread() || !_resizeRequest || size.width < 1.0 ||
        size.height < 1.0)
        return NO;
    const BOOL previous = _applyingEditorSize;
    _applyingEditorSize = YES;
    const BOOL accepted = _resizeRequest(size);
    _applyingEditorSize = previous;
    if (accepted)
        [self setFrameWithoutEcho:size];
    return accepted;
}

- (void)hostResizedTo:(CGSize)size {
    if (_applyingEditorSize || !_session || size.width < 1.0 || size.height < 1.0)
        return;
    _session->set_size(static_cast<std::uint32_t>(size.width),
                       static_cast<std::uint32_t>(size.height));
}

- (void)pushScale {
    if (!_session || self.window == nil)
        return;
#if TARGET_OS_OSX
    _session->set_scale(self.window.backingScaleFactor);
#else
    const CGFloat scale = self.window.screen.scale;
    if (scale > 0.0 && scale != _pushedScale) {
        _pushedScale = scale;
        _session->set_scale(scale);
    }
#endif
}

#if TARGET_OS_OSX

- (BOOL)isFlipped {
    return YES;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self hostResizedTo:NSSizeToCGSize(newSize)];
}

- (void)viewWillMoveToWindow:(NSWindow*)window {
    [super viewWillMoveToWindow:window];
    if (_session)
        _session->visibility_changed(window != nil);
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self pushScale];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self pushScale];
}

- (BOOL)offerKey:(NSEvent*)event down:(BOOL)down {
    if (!_session)
        return NO;
    const auto key = pulp::format::detail::appkit_contract_key_event(
        event.keyCode, event.charactersIgnoringModifiers, event.modifierFlags, down,
        down && event.isARepeat);
    return _session->offer_key(key) == pulp::format::KeyDisposition::consumed;
}

- (void)keyDown:(NSEvent*)event {
    if (![self offerKey:event down:YES])
        [super keyDown:event]; // up the responder chain to the host
}

- (void)keyUp:(NSEvent*)event {
    if (![self offerKey:event down:NO])
        [super keyUp:event];
}

#else // iOS

- (void)layoutSubviews {
    [super layoutSubviews];
    [self hostResizedTo:self.bounds.size];
    [self pushScale];
}

- (void)willMoveToWindow:(UIWindow*)window {
    [super willMoveToWindow:window];
    if (_session)
        _session->visibility_changed(window != nil);
}

- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self pushScale];
}

#endif

@end
