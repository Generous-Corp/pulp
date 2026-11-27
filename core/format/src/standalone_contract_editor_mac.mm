// macOS host for a standalone app's contract editor
// (detail/standalone_contract_editor.hpp): one NSWindow whose content view is
// the editor's parent, driven by a plain event loop that also ticks the editor.

#include <pulp/platform/test_visibility.hpp>
#include <pulp/format/detail/standalone_contract_editor.hpp>

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/detail/appkit_contract_key.hpp>
#include <pulp/format/detail/editor_size_constraint.hpp>
#include <pulp/format/detail/standalone_key_driver.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone_panel.hpp>
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
// the session, offers keys before the host acts on them, and toggles the
// standalone's panels on their shortcuts.
@interface PulpStandaloneContractView : NSView {
    pulp::format::ContractEditorSession* _session;
    std::function<void()> _onTransportKey;
    std::vector<pulp::format::StandalonePanel*> _panels;
    bool _applyingEditorSize;
}
- (void)setSession:(pulp::format::ContractEditorSession*)session
    onTransportKey:(std::function<void()>)onTransportKey;
- (void)setPanels:(std::vector<pulp::format::StandalonePanel*>)panels;
- (bool)applyEditorSize:(NSSize)size;
@end

@implementation PulpStandaloneContractView

- (void)setSession:(pulp::format::ContractEditorSession*)session
    onTransportKey:(std::function<void()>)onTransportKey {
    _session = session;
    _onTransportKey = std::move(onTransportKey);
}

- (void)setPanels:(std::vector<pulp::format::StandalonePanel*>)panels {
    _panels = std::move(panels);
}

// A panel's shortcut is Command plus its letter, with no other modifier.
// AppKit offers a Command chord here before keyDown:, and a window without a
// menu (headless) has nothing else that would see it.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (event.type == NSEventTypeKeyDown && !event.isARepeat && !_panels.empty()) {
        const auto flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
                           (NSEventModifierFlagCommand | NSEventModifierFlagShift |
                            NSEventModifierFlagControl | NSEventModifierFlagOption);
        NSString* chars = event.charactersIgnoringModifiers.lowercaseString;
        if (flags == NSEventModifierFlagCommand && chars.length == 1) {
            const unichar typed = [chars characterAtIndex:0];
            for (auto* panel : _panels) {
                if (panel != nullptr && panel->shortcut() != 0 &&
                    typed == static_cast<unichar>(panel->shortcut())) {
                    panel->toggle();
                    return YES;
                }
            }
        }
    }
    return [super performKeyEquivalent:event];
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

// The target of the app menu's items: each item's tag indexes the menu model.
@interface PulpStandaloneContractMenuTarget : NSObject {
  @public
    std::vector<pulp::format::detail::StandaloneContractMenuItem> items;
}
- (void)invokeItem:(NSMenuItem*)sender;
@end

@implementation PulpStandaloneContractMenuTarget
- (void)invokeItem:(NSMenuItem*)sender {
    const auto index = static_cast<std::size_t>(sender.tag);
    if (index < items.size() && items[index].action)
        items[index].action();
}
@end

namespace pulp::format::detail {

namespace {

// The app menu as a model: "Check for Updates…" when the updater offers it,
// "Audio/MIDI Settings…" (Cmd+,), "<name> Settings…" (Cmd+Option+,) for the
// plug-in's settings editor, one item per panel on its shortcut, and a Quit
// that closes the editor window, so the editor detaches in order. Built for
// every run, headless included, so a test reads the same items the window
// installs.
std::vector<StandaloneContractMenuItem>
build_app_menu_model(const StandaloneContractEditorOptions& options,
                     std::function<bool()> open_plugin_settings, const std::string& name,
                     NSWindow* window) {
    std::vector<StandaloneContractMenuItem> items;
    if (options.on_check_for_updates)
        items.push_back({StandaloneContractMenuItem::Role::updates, "Check for Updates\u2026", "",
                         false, options.on_check_for_updates});
    if (options.on_open_settings) {
        auto open = options.on_open_settings;
        items.push_back({StandaloneContractMenuItem::Role::audio_settings,
                         "Audio/MIDI Settings\u2026", ",", false, [open] { open(); }});
    }
    if (open_plugin_settings)
        items.push_back({StandaloneContractMenuItem::Role::plugin_settings,
                         name + " Settings\u2026", ",", true,
                         [open = std::move(open_plugin_settings)] { open(); }});
    for (auto* panel : options.panels)
        items.push_back({StandaloneContractMenuItem::Role::panel, panel->menu_title(),
                         std::string(1, panel->shortcut()), false,
                         [panel] { panel->toggle(); }});
    items.push_back({StandaloneContractMenuItem::Role::quit, "Quit " + name, "q", false,
                     [window] { [window performClose:nil]; }});
    return items;
}

// Install the model as the app's main menu. Skipped when the process already
// has one.
bool install_app_menu(PulpStandaloneContractMenuTarget* target, NSString* title) {
    if ([NSApp mainMenu] != nil)
        return false;
    NSMenu* bar = [[[NSMenu alloc] init] autorelease];
    NSMenuItem* appItem = [[[NSMenuItem alloc] init] autorelease];
    [bar addItem:appItem];
    NSMenu* appMenu = [[[NSMenu alloc] initWithTitle:title] autorelease];
    for (std::size_t i = 0; i < target->items.size(); ++i) {
        const auto& model = target->items[i];
        using Role = StandaloneContractMenuItem::Role;
        // Quit sits below a separator.
        if (model.role == Role::quit && i > 0)
            [appMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem* item = [[[NSMenuItem alloc]
            initWithTitle:[NSString stringWithUTF8String:model.title.c_str()]
                   action:@selector(invokeItem:)
            keyEquivalent:[NSString stringWithUTF8String:model.key.c_str()]] autorelease];
        if (model.option_modifier)
            item.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
        item.tag = static_cast<NSInteger>(i);
        item.target = target;
        [appMenu addItem:item];
        // So does the updater's item from the settings and panels after it.
        if (model.role == Role::updates && i + 1 < target->items.size() &&
            target->items[i + 1].role != Role::quit)
            [appMenu addItem:[NSMenuItem separatorItem]];
    }
    appItem.submenu = appMenu;
    [NSApp setMainMenu:bar];
    return true;
}

// The plug-in's own settings (EditorRole::settings) in a second window. Opened
// on demand; the editor window's loop ticks it and closes it before the main
// editor detaches. Headless runs build it without ever showing it.
class PluginSettingsWindow {
  public:
    PluginSettingsWindow(Processor& processor, state::StateStore& store, EditorFactory& factory,
                         std::string title, bool headless)
        : processor_(processor), store_(store), factory_(factory), title_(std::move(title)),
          headless_(headless) {}
    ~PluginSettingsWindow() {
        close();
    }

    // Open it, or bring it forward when it is already open. False when the
    // factory builds no settings editor or it does not attach.
    bool open() {
        if (window_ != nil) {
            if (!headless_)
                [window_ makeKeyAndOrderFront:nil];
            return true;
        }
        ContractEditorSession::HostHooks hooks;
        hooks.request_resize = [this](const EditorSize& size) {
            if (content_ == nil)
                return false;
            return [content_ applyEditorSize:NSMakeSize(size.width, size.height)];
        };
        hooks.host_action = [this](const HostActionRequest& request) {
            if (request.action != HostAction::close_editor || window_ == nil)
                return false;
            [window_ performClose:nil];
            return true;
        };
        session_ = ContractEditorSession::create(processor_, store_, factory_, std::move(hooks),
                                                 ContractEditorSession::IdleSource::host_driven,
                                                 EditorRole::settings);
        if (!session_) {
            runtime::log_error("Standalone: the editor factory built no settings editor");
            return false;
        }
        const auto size = session_->size();
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
        if (session_->resizable())
            style |= NSWindowStyleMaskResizable;
        window_ = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        window_.releasedWhenClosed = NO;
        window_.title = [NSString stringWithUTF8String:title_.c_str()];
        content_ = [[PulpStandaloneContractView alloc]
            initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
        window_.contentView = content_;
        delegate_ = [[PulpStandaloneContractWindowDelegate alloc] init];
        delegate_->session = session_.get();
        window_.delegate = delegate_;
        NativeParent parent;
        parent.kind = NativeParentKind::cocoa_view;
        parent.handle = (__bridge void*)content_;
        if (!session_->attach(parent)) {
            runtime::log_error("Standalone: the settings editor did not attach");
            close();
            return false;
        }
        [content_ setSession:session_.get() onTransportKey:{}];
        session_->set_scale(window_.backingScaleFactor);
        if (!headless_) {
            session_->visibility_changed(true);
            [window_ center];
            pulp::platform::order_in_window(window_);
        }
        return true;
    }

    bool is_open() const {
        return window_ != nil;
    }

    // One frame: tick while open, and close once the user closed the window.
    void tick() {
        if (window_ == nil)
            return;
        if (delegate_->closed) {
            close();
            return;
        }
        session_->tick();
    }

    // An orderly close: the window still exists while the editor detaches.
    void close() {
        if (window_ == nil)
            return;
        if (session_) {
            if (!headless_)
                session_->visibility_changed(false);
            session_->detach();
        }
        [content_ setSession:nullptr onTransportKey:{}];
        delegate_->session = nullptr;
        session_.reset();
        window_.delegate = nil;
        [delegate_ release];
        delegate_ = nil;
        [content_ release];
        content_ = nil;
        [window_ orderOut:nil];
        [window_ release];
        window_ = nil;
    }

  private:
    Processor& processor_;
    state::StateStore& store_;
    EditorFactory& factory_;
    std::string title_;
    bool headless_;
    std::unique_ptr<ContractEditorSession> session_;
    NSWindow* window_ = nil;
    PulpStandaloneContractView* content_ = nil;
    PulpStandaloneContractWindowDelegate* delegate_ = nil;
};

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
    hooks.host_action = [&window, &options](const HostActionRequest& request) {
        if (request.action == HostAction::open_settings)
            return options.on_open_settings ? options.on_open_settings() : false;
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
    [content setPanels:options.panels];
    for (auto* panel : options.panels)
        panel->set_primary_window((__bridge void*)window);
    session->set_scale(window.backingScaleFactor);
    [window makeFirstResponder:content];
    if (options.on_window_ready)
        options.on_window_ready((__bridge void*)window);
    // The plug-in's own settings, when its factory offers a settings editor.
    std::unique_ptr<PluginSettingsWindow> plugin_settings;
    if (factory.supports(EditorRole::settings)) {
        plugin_settings = std::make_unique<PluginSettingsWindow>(
            processor, store, factory, processor.descriptor().name + " Settings", options.headless);
    }
    std::function<bool()> open_plugin_settings;
    if (plugin_settings) {
        auto* settings_window = plugin_settings.get();
        open_plugin_settings = [settings_window] { return settings_window->open(); };
    }
    auto menu_items = build_app_menu_model(options, std::move(open_plugin_settings),
                                           processor.descriptor().name, window);
    if (options.test_on_app_menu)
        options.test_on_app_menu(menu_items);
    PulpStandaloneContractMenuTarget* menu_target = nil;
    bool installed_menu = false;
    // A Quit alone is no reason to take over the app's menu.
    if (!options.headless && menu_items.size() > 1) {
        menu_target = [[PulpStandaloneContractMenuTarget alloc] init];
        menu_target->items = std::move(menu_items);
        installed_menu = install_app_menu(
            menu_target, [NSString stringWithUTF8String:processor.descriptor().name.c_str()]);
    }

    if (!options.headless) {
        session->visibility_changed(true);
        [window center];
        pulp::platform::order_in_window(window);
        pulp::platform::activate_app_for_window();
    }

    bool ok = true;
    int frame = 0;
    std::size_t next_key = 0;
    const int key_delay = options.key_frame_delay > 0 ? options.key_frame_delay : 1;
    int capture_frame = -1;
    while (!delegate->closed) {
        pump_one_frame();
        session->tick();
        if (plugin_settings) {
            if (frame == options.test_open_plugin_settings_frame)
                plugin_settings->open();
            plugin_settings->tick();
        }
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

    // The settings editor goes first, while the main editor is still attached.
    plugin_settings.reset();
    // An orderly close: the window still exists while the editor detaches.
    if (!options.headless)
        session->visibility_changed(false);
    session->detach();
    [content setSession:nullptr onTransportKey:{}];
    [content setPanels:{}];
    for (auto* panel : options.panels)
        panel->set_primary_window(nullptr);
    delegate->session = nullptr;
    session.reset();
    window.delegate = nil;
    [delegate release];
    if (menu_target != nil) {
        if (installed_menu)
            [NSApp setMainMenu:nil];
        menu_target->items.clear();
        [menu_target release];
    }
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
