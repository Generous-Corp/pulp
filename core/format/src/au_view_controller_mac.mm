// macOS AUv3 view controller — provides an AUViewController (NSViewController
// on macOS) for AU v3 app extensions with a custom GPU/Skia editor.
//
// Symmetric to `au_view_controller_ios.mm`: both controllers implement
// AUAudioUnitFactory, instantiate `PulpAudioUnit` from the factory entry,
// open a `ViewBridge` against the same Processor / StateStore the audio
// render block runs against, and attach a `PluginViewHost` to the platform
// view. `set_design_viewport` + `set_fixed_aspect_ratio` deliver the same
// proportional / aspect-locked resize behavior Phase 3 added for standalone,
// VST3, and CLAP.

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_OSX

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>

#include "pulp_audio_unit_fwd.h"

#include <algorithm>
#include <memory>

/// AUViewController subclass + AUAudioUnitFactory for macOS AU v3.
/// Apple's current pattern: the view controller IS the factory. Info.plist
/// sets NSExtensionPrincipalClass = PulpAUMacViewController and
/// NSExtensionPointIdentifier = com.apple.AudioUnit-UI.
@interface PulpAUMacViewController : AUViewController <AUAudioUnitFactory>
@property (nonatomic, strong) AUAudioUnit *audioUnit;
@end

@implementation PulpAUMacViewController {
    // Ivar order is load-bearing — see -dealloc in
    // au_view_controller_ios.mm for the same contract. _viewHost holds
    // `View& root_` and MUST be destroyed before whichever object owns
    // the View (bridge view OR fallback view). Declaring _viewHost LAST
    // makes it destroy FIRST (reverse ivar order).
    std::unique_ptr<pulp::format::ViewBridge> _bridge;
    std::unique_ptr<pulp::view::View> _fallbackView;
    std::unique_ptr<pulp::view::PluginViewHost> _viewHost;
}

- (void)loadView {
    // We're not loading from a NIB — create the root NSView ourselves so
    // `viewDidLoad` finds a sized container even before the host attaches
    // us to a window.
    NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    root.wantsLayer = YES;
    root.layer.backgroundColor = NSColor.blackColor.CGColor;
    self.view = root;
#if !__has_feature(objc_arc)
    [root release];
#endif
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.preferredContentSize = NSMakeSize(400, 300);
    [self rebuildEditorIfReady];
}

- (void)setAudioUnit:(AUAudioUnit *)audioUnit {
    if (_audioUnit == audioUnit) return;
#if !__has_feature(objc_arc)
    [_audioUnit release];
    _audioUnit = [audioUnit retain];
#else
    _audioUnit = audioUnit;
#endif
    // The factory method runs on the XPC connection's serial queue, not
    // main. NSViewController APIs (setPreferredContentSize, self.view, the
    // PluginViewHost AppKit attach) require the main thread or they throw
    // NSInternalInconsistencyException → the .appex process crashes and
    // Logic reports "Failed to load Audio Unit". Bounce to main; dispatch
    // async so we don't deadlock if we ARE already on main (e.g. from
    // viewDidLoad).
    if ([NSThread isMainThread]) {
        [self rebuildEditorIfReady];
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self rebuildEditorIfReady];
        });
    }
}

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error {
    // Apple AUAudioUnitFactory contract: the factory's
    // createAudioUnitWithComponentDescription:error: is what instantiates
    // the AU. Returning self.audioUnit unconditionally (as the old iOS
    // path did) leaves the AU nil and the host opens a generic-view
    // editor over a dead AU.
    //
    // Threading: this method runs on the XPC connection queue. Constructing
    // PulpAudioUnit is thread-safe (no AppKit). setAudioUnit: below will
    // bounce the view-building work to main.
    PulpAudioUnit *au = [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                                       error:error];
    if (!au) return nil;
#if !__has_feature(objc_arc)
    self.audioUnit = [au autorelease];
#else
    self.audioUnit = au;
#endif
    return self.audioUnit;
}

- (void)rebuildEditorIfReady {
    // HARD GUARD: this method touches NSViewController state
    // (preferredContentSize, self.view, the PluginViewHost AppKit attach)
    // which is main-thread-only. The host calls
    // createAudioUnitWithComponentDescription on the XPC connection queue
    // (com.apple.NSXPCConnection.user.endpoint); setAudioUnit: SHOULD
    // dispatch us to main, but compiler inlining can bypass the property
    // setter override. Re-bouncing here is the belt-and-suspenders that
    // proves the host won't crash on `setPreferredContentSize` no matter
    // which path called us.
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self rebuildEditorIfReady];
        });
        return;
    }

    if (pulp::format::detail::editor_launch_blocked_by_environment()) {
        pulp::runtime::log_info("AU mac editor: disabled in headless/CI/test environment");
        return;
    }

    if (!self.audioUnit) {
        // Factory entry hasn't been called yet. We'll be re-invoked from
        // setAudioUnit:.
        return;
    }
    if (!self.isViewLoaded) {
        // View hasn't been built yet — viewDidLoad will re-invoke us.
        return;
    }

    // Drop the previous editor first, in destruction order (host → fallback
    // → bridge) so we can rebuild against a fresh ViewBridge.
    _viewHost.reset();
    _fallbackView.reset();
    _bridge.reset();

    pulp::format::Processor *processor = nullptr;
    pulp::state::StateStore *store = nullptr;
    if ([self.audioUnit respondsToSelector:@selector(pulpProcessor)] &&
        [self.audioUnit respondsToSelector:@selector(pulpStore)]) {
        processor = [(PulpAudioUnit *)self.audioUnit pulpProcessor];
        store = [(PulpAudioUnit *)self.audioUnit pulpStore];
    }

    pulp::view::View *root = nullptr;
    uint32_t w = 400, h = 300;
    if (processor && store) {
        _bridge = std::make_unique<pulp::format::ViewBridge>(
            *processor, *store,
            pulp::format::ViewBridge::Options{.enable_hot_reload = false,
                                              .role = pulp::format::ViewRole::Editor});
        std::string err;
        if (!_bridge->open(&err)) {
            pulp::runtime::log_error("AU mac: ViewBridge::open failed ({})", err);
            _bridge.reset();
        } else {
            root = _bridge->view();
            w = _bridge->size_hints().preferred_width;
            h = _bridge->size_hints().preferred_height;
        }
    }

    if (!root) {
        _fallbackView = std::make_unique<pulp::view::View>();
        root = _fallbackView.get();
    }

    self.preferredContentSize = NSMakeSize(w, h);

    pulp::view::PluginViewHost::Options opts;
    opts.size = {w, h};
    const char *mode = "fallback";
    if (_bridge) {
        const auto gpu = pulp::format::decide_gpu_host(*_bridge);
        opts.use_gpu = gpu.use_gpu;
        mode = gpu.mode;
        _viewHost = pulp::view::PluginViewHost::create(*root, opts);
        if (_viewHost) {
            pulp::format::warn_if_unexpected_cpu_fallback(gpu, _viewHost.get());
            _viewHost->set_idle_callback(pulp::format::make_scripted_idle_pump(*_bridge));
        }
    } else {
        _viewHost = pulp::view::PluginViewHost::create(*root, opts);
    }

    if (!_viewHost) {
        pulp::runtime::log_error("AU mac: PluginViewHost::create returned null");
        return;
    }

    // Phase 3 viewport pin + aspect lock: paint at design size; let the
    // host size the window. The host receives the *container* size in
    // resize(), not the design size — PluginViewHost::set_design_viewport
    // scales the paint/input transform.
    if (w > 0 && h > 0) {
        _viewHost->set_design_viewport(w, h);
        _viewHost->set_fixed_aspect_ratio(static_cast<float>(w) /
                                          static_cast<float>(h));
    }

    _viewHost->attach_to_parent((__bridge void *)self.view);
    if (_bridge) _bridge->notify_attached();

    pulp::runtime::log_info("AU mac: editor attached, design={}x{}, mode={}, gpu={}",
                            w, h, mode, _viewHost->is_gpu_backed());

    [self resizeEditorToViewBounds];
}

- (void)resizeEditorToViewBounds {
    if (!_viewHost) return;
    NSSize size = self.view.bounds.size;
    const uint32_t w = std::max(1u, static_cast<uint32_t>(size.width));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(size.height));
    _viewHost->set_size(w, h);
    if (_bridge) _bridge->resize(w, h);
}

- (void)viewDidLayout {
    [super viewDidLayout];
    [self resizeEditorToViewBounds];
}

- (void)viewWillTransitionToSize:(NSSize)newSize {
    // AUViewController on macOS may not always route through this selector —
    // viewDidLayout is the authority. This is a belt-and-suspenders for hosts
    // that pre-announce a size change before the layout pass.
    if (!_viewHost) return;
    const uint32_t w = std::max(1u, static_cast<uint32_t>(newSize.width));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(newSize.height));
    _viewHost->set_size(w, h);
    if (_bridge) _bridge->resize(w, h);
}

#if !__has_feature(objc_arc)
- (void)dealloc {
    // Same destruction-order contract as au_view_controller_ios.mm:
    // ivars destroy in reverse declaration order after [super dealloc].
    // 1. ~PluginViewHost runs first, clears back-pointer on root_ View.
    // 2. ~unique_ptr<View> (_fallbackView) — no-op on success path.
    // 3. ~ViewBridge — close() fires on_view_closed, releases scripted UI.
    // Explicitly closing the bridge HERE would reverse that order and
    // dereference a dangling root_ from ~PluginViewHost. Don't.
    [_audioUnit release];
    [super dealloc];
}
#endif

@end

#endif // TARGET_OS_OSX
#endif // __APPLE__
