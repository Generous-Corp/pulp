// macOS host for a standalone app's contract editor
// (detail/standalone_contract_editor.hpp): one NSWindow whose content view is
// the editor's parent, driven by a plain event loop that also ticks the editor.

#include <pulp/format/detail/standalone_contract_editor.hpp>

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/detail/appkit_contract_key.hpp>
#include <pulp/format/detail/editor_size_constraint.hpp>
#include <pulp/format/detail/standalone_key_driver.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>

#import <AppKit/AppKit.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// The window's content view: the editor's parent. It forwards host resizes to
// the session and offers keys before the host acts on them.
@interface PulpStandaloneContractView : NSView {
    pulp::format::ContractEditorSession* _session;
    std::function<void()> _onTransportKey;
    bool _applyingEditorSize;
}
- (void)setSession:(pulp::format::ContractEditorSession*)session
    onTransportKey:(std::function<void()>)onTransportKey;
- (bool)applyEditorSize:(NSSize)size;
@end

@implementation PulpStandaloneContractView

- (void)setSession:(pulp::format::ContractEditorSession*)session
    onTransportKey:(std::function<void()>)onTransportKey {
    _session = session;
    _onTransportKey = std::move(onTransportKey);
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (bool)applyEditorSize:(NSSize)size {
    if (_session == nullptr || self.window == nil)
        return false;
    auto width = static_cast<std::uint32_t>(size.width);
    auto height = static_cast<std::uint32_t>(size.height);
    bool snapped = false;
    // Deliver the editor-origin acknowledgment before changing the window.
    // Refusal leaves both the native window and the editor at their old size.
    if (!_session->set_host_size(width, height, snapped))
        return false;
    _applyingEditorSize = true;
    [self.window setContentSize:NSMakeSize(width, height)];
    _applyingEditorSize = false;
    return true;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (_applyingEditorSize || _session == nullptr || newSize.width < 1.0 || newSize.height < 1.0)
        return;
    auto width = static_cast<std::uint32_t>(newSize.width);
    auto height = static_cast<std::uint32_t>(newSize.height);
    // A size the window took without a drag (a programmatic resize, full
    // screen, a restored frame) still goes through the editor's limits and
    // aspect: the editor takes the constrained size now, and the window is
    // brought there through the session's snapped-size request once the
    // window's sizes settle.
    if (_session->editor() != nullptr) {
        bool snapped = false;
        _session->set_host_size(width, height, snapped);
    } else {
        _session->set_size(width, height);
    }
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_session != nullptr && self.window != nil)
        _session->set_scale(self.window.backingScaleFactor);
}

// True when the editor or the host took the key.
- (bool)offer:(NSEvent*)event down:(bool)down {
    if (_session == nullptr)
        return false;
    const auto key = pulp::format::detail::appkit_contract_key_event(
        event.keyCode, event.charactersIgnoringModifiers, event.modifierFlags, down,
        down && event.isARepeat);
    if (_session->offer_key(key) == pulp::format::KeyDisposition::consumed)
        return true;
    // Forwarded: the host's own shortcuts. Space is the transport key.
    if (key.key == pulp::format::EditorKey::space && key.modifiers == 0) {
        if (down && !key.is_repeat && _onTransportKey)
            _onTransportKey();
        return true;
    }
    return false;
}

- (void)keyDown:(NSEvent*)event {
    if (![self offer:event down:true])
        [super keyDown:event];
}

- (void)keyUp:(NSEvent*)event {
    if (![self offer:event down:false])
        [super keyUp:event];
}

@end

// Ends the loop when the user closes the window, and holds a live resize to
// the editor's limits and locked aspect.
@interface PulpStandaloneContractWindowDelegate : NSObject <NSWindowDelegate> {
  @public
    bool closed;
    pulp::format::ContractEditorSession* session;
}
@end

@implementation PulpStandaloneContractWindowDelegate
- (NSSize)windowWillResize:(NSWindow*)sender toSize:(NSSize)frameSize {
    if (session == nullptr || session->editor() == nullptr || !session->resizable())
        return frameSize;
    const NSRect content =
        [sender contentRectForFrameRect:NSMakeRect(0, 0, frameSize.width, frameSize.height)];
    if (content.size.width < 1.0 || content.size.height < 1.0)
        return frameSize;
    auto width = static_cast<std::uint32_t>(std::lround(content.size.width));
    auto height = static_cast<std::uint32_t>(std::lround(content.size.height));
    if (!session->constrain(width, height))
        return frameSize;
    return [sender frameRectForContentRect:NSMakeRect(0, 0, width, height)].size;
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    closed = true;
}
@end

namespace pulp::format::detail {

namespace {

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// One pass of the event loop: dispatch whatever arrived within ~a frame.
void pump_one_frame() {
    @autoreleasepool {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:1.0 / 60.0];
        while (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                   untilDate:until
                                                      inMode:NSDefaultRunLoopMode
                                                     dequeue:YES]) {
            [NSApp sendEvent:event];
            until = [NSDate distantPast];
        }
    }
}

} // namespace

bool standalone_contract_editor_supported() {
    return true;
}

bool run_standalone_contract_editor(Processor& processor, state::StateStore& store,
                                    EditorFactory& factory,
                                    const StandaloneContractEditorOptions& options) {
    if (options.headless && options.screenshot_path.empty()) {
        runtime::log_error("Standalone: a headless contract editor needs a screenshot path");
        return false;
    }

    [NSApplication sharedApplication];
    if (!options.headless) {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];
    }

    NSWindow* window = nil;
    PulpStandaloneContractView* content = nil;

    ContractEditorSession::HostHooks hooks;
    hooks.request_resize = [&content](const EditorSize& size) {
        if (content == nil)
            return false;
        return [content applyEditorSize:NSMakeSize(size.width, size.height)];
    };
    hooks.host_action = [&window](const HostActionRequest& request) {
        if (request.action != HostAction::close_editor || window == nil)
            return false;
        [window performClose:nil];
        return true;
    };
    // The window drives the tick, so there is no second clock to stop.
    auto session = ContractEditorSession::create(processor, store, factory, std::move(hooks),
                                                 ContractEditorSession::IdleSource::host_driven);
    if (!session) {
        runtime::log_error("Standalone: the editor factory built no editor");
        return false;
    }

    auto size = session->size();
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (session->resizable())
        style |= NSWindowStyleMaskResizable;
    // Open inside the screen: shrink an oversized editor, keeping its aspect
    // and limits, by the room left after the window chrome.
    {
        double visible_width = options.screen_visible_width;
        double visible_height = options.screen_visible_height;
        if (visible_width <= 0.0 || visible_height <= 0.0) {
            const NSRect visible = [NSScreen mainScreen].visibleFrame;
            visible_width = visible.size.width;
            visible_height = visible.size.height;
        }
        const NSRect chrome = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, 100, 100)
                                                      styleMask:style];
        const double room_width = visible_width - (chrome.size.width - 100.0);
        const double room_height = visible_height - (chrome.size.height - 100.0);
        if (room_width >= 1.0 && room_height >= 1.0) {
            auto width = size.width;
            auto height = size.height;
            detail::fit_size_to_area(session->size_hints(), width, height,
                                     static_cast<std::uint32_t>(room_width),
                                     static_cast<std::uint32_t>(room_height));
            if (width != size.width || height != size.height) {
                session->set_size(width, height);
                size = session->size();
            }
        }
    }
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                         styleMask:style
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    window.releasedWhenClosed = NO;
    window.title = [NSString stringWithUTF8String:options.title.c_str()];
    const auto hints = session->size_hints();
    if (hints.min_width > 0 && hints.min_height > 0)
        window.contentMinSize = NSMakeSize(hints.min_width, hints.min_height);
    if (hints.max_width > 0 && hints.max_height > 0)
        window.contentMaxSize = NSMakeSize(hints.max_width, hints.max_height);

    content = [[PulpStandaloneContractView alloc]
        initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    window.contentView = content;
    [content release];
    PulpStandaloneContractWindowDelegate* delegate =
        [[PulpStandaloneContractWindowDelegate alloc] init];
    delegate->session = session.get();
    window.delegate = delegate;

    NativeParent parent;
    parent.kind = NativeParentKind::cocoa_view;
    parent.handle = (__bridge void*)content;
    if (!session->attach(parent)) {
        window.delegate = nil;
        [delegate release];
        [window release];
        return false;
    }
    [content setSession:session.get() onTransportKey:options.on_transport_key];
    session->set_scale(window.backingScaleFactor);
    [window makeFirstResponder:content];
    if (options.on_window_ready)
        options.on_window_ready((__bridge void*)window);

    if (!options.headless) {
        session->visibility_changed(true);
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }

    bool ok = true;
    int frame = 0;
    std::size_t next_key = 0;
    const int key_delay = options.key_frame_delay > 0 ? options.key_frame_delay : 1;
    int capture_frame = -1;
    while (!delegate->closed) {
        pump_one_frame();
        session->tick();
        if (options.on_idle)
            options.on_idle();
        ++frame;
        if (!options.headless)
            continue;
        if (next_key < options.key_steps.size()) {
            if (frame >= key_delay * static_cast<int>(next_key + 1)) {
                if (!deliver_synthetic_key((__bridge void*)content, options.key_steps[next_key])) {
                    runtime::log_error("Standalone: could not deliver key '{}'",
                                       options.key_steps[next_key].label);
                    ok = false;
                    break;
                }
                ++next_key;
            }
            continue;
        }
        if (capture_frame < 0)
            capture_frame =
                frame + (options.screenshot_frame_delay > 0 ? options.screenshot_frame_delay : 1);
        if (frame < capture_frame)
            continue;
        std::vector<std::uint8_t> png;
        auto* editor = session->editor();
        auto* automation = editor != nullptr ? editor->automation() : nullptr;
        if (automation == nullptr || !automation->capture_png(png) || png.empty()) {
            runtime::log_error("Standalone: the editor offers no frame capture");
            ok = false;
        } else if (!write_file(options.screenshot_path, png)) {
            runtime::log_error("Standalone: could not write {}", options.screenshot_path);
            ok = false;
        } else {
            runtime::log_info("Standalone: wrote {}", options.screenshot_path);
        }
        break;
    }

    // An orderly close: the window still exists while the editor detaches.
    if (!options.headless)
        session->visibility_changed(false);
    session->detach();
    [content setSession:nullptr onTransportKey:{}];
    delegate->session = nullptr;
    session.reset();
    window.delegate = nil;
    [delegate release];
    [window orderOut:nil];
    [window release];
    return ok;
}

} // namespace pulp::format::detail

#else // TARGET_OS_OSX

#include <pulp/runtime/log.hpp>

namespace pulp::format::detail {

bool standalone_contract_editor_supported() {
    return false;
}

bool run_standalone_contract_editor(Processor&, state::StateStore&, EditorFactory&,
                                    const StandaloneContractEditorOptions&) {
    runtime::log_error("Standalone: this platform cannot host an editor-host contract editor yet");
    return false;
}

} // namespace pulp::format::detail

#endif // TARGET_OS_OSX
