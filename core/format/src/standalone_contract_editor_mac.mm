// macOS host for a standalone app's contract editor
// (detail/standalone_contract_editor.hpp): one NSWindow whose content view is
// the editor's parent, driven by a plain event loop that also ticks the editor.

#include <pulp/format/detail/standalone_contract_editor.hpp>

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/detail/appkit_contract_key.hpp>
#include <pulp/format/detail/standalone_key_driver.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>

#import <AppKit/AppKit.h>

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
- (void)applyEditorSize:(NSSize)size;
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

- (void)applyEditorSize:(NSSize)size {
    _applyingEditorSize = true;
    [self.window setContentSize:size];
    _applyingEditorSize = false;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (_applyingEditorSize || _session == nullptr || newSize.width < 1.0 || newSize.height < 1.0)
        return;
    _session->set_size(static_cast<std::uint32_t>(newSize.width),
                       static_cast<std::uint32_t>(newSize.height));
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

// Ends the loop when the user closes the window.
@interface PulpStandaloneContractWindowDelegate : NSObject <NSWindowDelegate> {
  @public
    bool closed;
}
@end

@implementation PulpStandaloneContractWindowDelegate
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
        [content applyEditorSize:NSMakeSize(size.width, size.height)];
        return true;
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

    const auto size = session->size();
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (session->resizable())
        style |= NSWindowStyleMaskResizable;
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
