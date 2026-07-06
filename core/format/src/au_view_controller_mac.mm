// macOS AUv3 view controller — provides an AUViewController (NSViewController
// on macOS) for AU v3 app extensions with a custom GPU/Skia editor.
//
// Symmetric to `au_view_controller_ios.mm`: both controllers implement
// AUAudioUnitFactory, instantiate `PulpAudioUnit` from the factory entry,
// open a `ViewBridge` against the same Processor / StateStore the audio
// render block runs against, and attach a `PluginViewHost` to the platform
// view. `set_design_viewport` + `set_fixed_aspect_ratio` deliver the same
// proportional / aspect-locked resize behavior as standalone, VST3, and CLAP.

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

#import "au_audio_unit.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

NSSize pulp_auv3_initial_editor_size() {
#if defined(PULP_PLUGIN_DESIGN_W) && defined(PULP_PLUGIN_DESIGN_H)
    if (PULP_PLUGIN_DESIGN_W > 0 && PULP_PLUGIN_DESIGN_H > 0) {
        return NSMakeSize(static_cast<CGFloat>(PULP_PLUGIN_DESIGN_W),
                          static_cast<CGFloat>(PULP_PLUGIN_DESIGN_H));
    }
#endif
    return NSMakeSize(400, 300);
}

void pulp_auv3_apply_preferred_size(NSViewController *controller,
                                    NSSize size,
                                    bool resize_view_frame) {
    if (!controller || size.width <= 0.0 || size.height <= 0.0) return;
    controller.preferredContentSize = size;
    if (!resize_view_frame || !controller.isViewLoaded) return;

    NSView *view = controller.view;
    NSRect frame = view.frame;
    frame.size = size;
    [view setFrame:frame];
    [view setNeedsLayout:YES];
}

// Initial-attach size-sync ticks. Logic hosts AU v3 out-of-process and the
// extension's `viewDidLayout` does not reliably fire when the host resizes the
// remote view, so the editor can paint its first frame at the design size
// inside whatever (often remembered) window Logic restored — clipping the UI
// until a manual resize. We poll the view's real bounds for a short window
// after attach and re-fit each tick. 8 × 60ms ≈ 0.5s covers Logic's late
// layout settle without lingering long enough to fight a deliberate resize.
static constexpr unsigned long kInitialSizeSyncMaxAttempts = 8;
static constexpr int64_t kInitialSizeSyncIntervalMs = 60;

} // namespace

/// Root view that reports host-driven frame changes synchronously.
///
/// In Logic Pro's out-of-process AU v3 hosting the host sets the remote view's
/// frame geometry to its (often restored) window size WITHOUT reliably driving
/// the controller's `viewDidLayout`. Polling `self.view.bounds` after attach
/// therefore misses the host's initial size and the editor's first frame stays
/// painted at the design size inside a smaller window (clipped) until the user
/// nudges it. Overriding `setFrameSize:` is the one hook guaranteed to fire on
/// every host-driven (re)size, so we re-fit the design viewport from here.
@interface PulpAUMacRootView : NSView
@property (nonatomic, copy) void (^onResize)(void);
@end

@implementation PulpAUMacRootView
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (self.onResize) self.onResize();
}
- (void)viewDidMoveToSuperview {
    [super viewDidMoveToSuperview];
    NSView *sv = self.superview;
    if (!sv) return;
    // Fill the host's container view so we pick up its real size — but do it the
    // FRAME-based way (springs & struts), NOT Auto Layout. Pinning with
    // NSLayoutConstraints (translatesAutoresizingMaskIntoConstraints=NO) crashed
    // Ableton Live: when the host places the AU window, our setFrameSize: →
    // [super] → setNeedsLayout engages the constraint engine
    // (-[NSWindow _postWindowNeedsLayout]) which throws in that context and the
    // uncaught exception kills the host. Frame-based autoresizing fills the
    // container identically for our purposes (the deferred GPU host then reads
    // these real bounds) without touching the constraint engine, and is
    // compatible with frame-driven AU hosts (Live, REAPER, Logic).
    self.translatesAutoresizingMaskIntoConstraints = YES;
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.frame = sv.bounds;
}
@end

/// AUViewController subclass + AUAudioUnitFactory for macOS AU v3.
/// Apple's current pattern: the view controller IS the factory. Info.plist
/// sets NSExtensionPrincipalClass = PulpAUMacViewController and
/// NSExtensionPointIdentifier = com.apple.AudioUnit-UI.
@interface PulpAUMacViewController : AUViewController <AUAudioUnitFactory>
@property (nonatomic, strong) AUAudioUnit *audioUnit;
@end

@implementation PulpAUMacViewController {
    NSSize _designSize;
    NSUInteger _initialSizeSyncAttempts;
    // Deferred GPU-host creation (Logic OOP first-paint fix): we hold the
    // resolved root View and wait to build the PluginViewHost (and its Dawn/
    // Skia surface) until the root view reports a real, settled host size, so
    // the surface is never born at the design size inside a smaller restored
    // Logic window. _pendingRoot points into _bridge / _fallbackView and is
    // only dereferenced before they are reset.
    pulp::view::View *_pendingRoot;
    BOOL _viewHostPending;
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
    // `viewDidLoad` finds a correctly sized container even before the host
    // attaches us to a window. REAPER's in-process AUv3 path can choose the
    // initial container from this frame before createAudioUnit has provided
    // the processor, so use compile-time design dimensions when available.
    const NSSize initial = pulp_auv3_initial_editor_size();
    PulpAUMacRootView *root =
        [[PulpAUMacRootView alloc] initWithFrame:NSMakeRect(0, 0, initial.width, initial.height)];
    root.wantsLayer = YES;
    root.layer.backgroundColor = NSColor.blackColor.CGColor;
    self.view = root;
#if !__has_feature(objc_arc)
    [root release];
#endif
}

- (void)viewDidLoad {
    [super viewDidLoad];
    _designSize = NSZeroSize;
    _initialSizeSyncAttempts = 0;
    pulp_auv3_apply_preferred_size(self, pulp_auv3_initial_editor_size(), true);
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

    // Publish the design size as the preferred size, but do NOT force the root
    // view's frame to it here. Forcing the frame during rebuild poisons the
    // first paint in Logic's OOP host: the editor paints at the design size
    // before Logic delivers its restored (smaller) window size, leaving the
    // first frame clipped. The PulpAUMacRootView `setFrameSize:` hook (wired
    // below) re-fits the design viewport to whatever size the host hands us —
    // including Logic's initial geometry push that `viewDidLayout` misses.
    const NSSize designSize = NSMakeSize(w, h);
    _designSize = designSize;
    _initialSizeSyncAttempts = 0;
    pulp_auv3_apply_preferred_size(self, designSize, /*resize_view_frame=*/false);

    // Defer PluginViewHost creation until the root view reports a real, settled
    // host size (see -createViewHostIfReady). Wire the frame-change hook first:
    // when Logic's OOP host finally sizes our (superview-pinned) view to its
    // restored window, this fires and builds the GPU host at THAT size — so the
    // first painted frame is already correct, never the design-size frame Logic
    // would otherwise composite (clipped) into a smaller window.
    _pendingRoot = root;
    _viewHostPending = YES;
    if ([self.view isKindOfClass:[PulpAUMacRootView class]]) {
        __unsafe_unretained PulpAUMacViewController *weakSelf = self;
        ((PulpAUMacRootView *)self.view).onResize = ^{
            [weakSelf createViewHostIfReady];
            [weakSelf resizeEditorToViewBounds];
        };
    }

    // Try immediately (REAPER's in-process path hands us a real size up front)
    // and start the settle driver as a fallback for hosts that size us late.
    [self createViewHostIfReady];
    [self scheduleInitialSizeSync];
}

// Build the GPU host lazily, at the view's REAL bounds, once the host has
// settled them. While the bounds still equal the design size we wait (up to the
// initial-size-sync window) for the host to deliver its possibly-smaller
// restored size; after that we accept the current size. Creating at the real
// size is what makes Logic's first paint correct instead of design-sized.
- (void)createViewHostIfReady {
    if (!_viewHostPending || !_pendingRoot) return;

    const NSSize b = self.view.bounds.size;
    if (b.width < 1.0 || b.height < 1.0) return;  // no real layout yet

    const bool boundsMatchDesign =
        std::abs(b.width - _designSize.width) < 1.0 &&
        std::abs(b.height - _designSize.height) < 1.0;
    if (boundsMatchDesign && _initialSizeSyncAttempts < kInitialSizeSyncMaxAttempts) {
        return;  // keep waiting for the host's settled (likely different) size
    }

    _viewHostPending = NO;

    const uint32_t w = static_cast<uint32_t>(_designSize.width);
    const uint32_t h = static_cast<uint32_t>(_designSize.height);

    pulp::view::PluginViewHost::Options opts;
    opts.size = {std::max(1u, static_cast<uint32_t>(b.width)),
                 std::max(1u, static_cast<uint32_t>(b.height))};  // REAL host size
    const char *mode = "fallback";
    if (_bridge) {
        const auto gpu = pulp::format::decide_gpu_host(*_bridge);
        opts.use_gpu = gpu.use_gpu;
        mode = gpu.mode;
        _viewHost = pulp::view::PluginViewHost::create(*_pendingRoot, opts);
        if (_viewHost) {
            pulp::format::warn_if_unexpected_cpu_fallback(gpu, _viewHost.get());
            _viewHost->set_idle_callback(pulp::format::make_scripted_idle_pump(*_bridge));
            // Hand the host's live GpuSurface to the scripted-UI session so
            // JS navigator.gpu / canvas.getContext('webgpu') routes through
            // Pulp's Dawn instance instead of mocks.
            if (auto* scripted = _bridge->scripted_ui()) {
                scripted->attach_gpu_surface(_viewHost->gpu_surface());
                if (_viewHost->gpu_surface()) {
                    pulp::runtime::log_info(
                        "[plugin-gpu-host] GpuSurface attached to WidgetBridge "
                        "via ScriptedUiSession (mac AUv3)");
                }
            }
        }
    } else {
        _viewHost = pulp::view::PluginViewHost::create(*_pendingRoot, opts);
    }

    if (!_viewHost) {
        pulp::runtime::log_error("AU mac: PluginViewHost::create returned null");
        _viewHostPending = YES;  // allow a later attempt
        return;
    }

    // Viewport pin + aspect lock: paint the design at the host size.
    if (w > 0 && h > 0) {
        _viewHost->set_design_viewport(w, h);
        _viewHost->set_fixed_aspect_ratio(static_cast<float>(w) /
                                          static_cast<float>(h));
        // Anchor the design to the TOP of the host pane. AU can't negotiate the
        // pane aspect the way CLAP (gui_adjust_size) / VST3 (checkSizeConstraint)
        // do, so a wide fixed design lands in a taller host pane (e.g. REAPER's
        // FX-chain pane). Top-aligning makes it read like CLAP/VST3 — content at
        // the top with the slack as a single bottom strip — instead of floating
        // centered between two bands.
        _viewHost->set_design_viewport_top_align(true);
    }

    _viewHost->attach_to_parent((__bridge void *)self.view);
    if (_bridge) _bridge->notify_attached();

    pulp::runtime::log_info("AU mac: editor attached at {}x{}, design={}x{}, mode={}, gpu={}",
                            static_cast<int>(b.width), static_cast<int>(b.height),
                            w, h, mode, _viewHost->is_gpu_backed());

    [self resizeEditorToViewBounds];
}

- (void)scheduleInitialSizeSync {
    if (_initialSizeSyncAttempts >= kInitialSizeSyncMaxAttempts) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        [self runInitialSizeSync];
    });
}

- (void)runInitialSizeSync {
    if (_designSize.width <= 0.0 || _designSize.height <= 0.0) return;
    if (_initialSizeSyncAttempts < kInitialSizeSyncMaxAttempts) {
        ++_initialSizeSyncAttempts;
    }

    // Settle driver / fallback for hosts that size us late: builds the deferred
    // GPU host once the view's bounds settle (or, on the final attempt, at
    // whatever size we have). We let the host own the window — no forced
    // window resize here; forcing Logic's window fights its restore behavior.
    // Once the host exists, just re-fit the design viewport.
    [self createViewHostIfReady];
    if (_viewHost) {
        [self resizeEditorToViewBounds];
    }

    if (_initialSizeSyncAttempts < kInitialSizeSyncMaxAttempts) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                     kInitialSizeSyncIntervalMs * NSEC_PER_MSEC),
                       dispatch_get_main_queue(), ^{
            [self runInitialSizeSync];
        });
    }
}

- (void)resizeEditorToViewBounds {
    if (!_viewHost) return;
    NSSize size = self.view.bounds.size;
    const uint32_t w = std::max(1u, static_cast<uint32_t>(size.width));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(size.height));
    _viewHost->set_size(w, h);
    if (_bridge) _bridge->resize(w, h);
    // Force a repaint at the new size. On Logic's first open the GPU surface
    // paints once at the design size before the superview-pin resolves the view
    // to Logic's container size; set_size alone updates the logical size but the
    // stale first frame can persist (clipped) until the host next requests a
    // redraw (a window reopen or a manual resize). Requesting the repaint here
    // makes the corrected size show on the first settle.
    _viewHost->repaint();
}

- (void)viewDidLayout {
    [super viewDidLayout];
    // Also a creation trigger: on hosts where viewDidLayout *does* fire with a
    // real size, build the deferred GPU host here too (idempotent — no-op once
    // created or while still waiting for a settled size).
    [self createViewHostIfReady];
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
    // Clear the root view's resize hook FIRST: it captures self unretained and
    // touches _viewHost (destroyed below, after [super dealloc]), so a stray
    // setFrameSize: during teardown must not call back into us.
    if ([self.view isKindOfClass:[PulpAUMacRootView class]]) {
        ((PulpAUMacRootView *)self.view).onResize = nil;
    }
    // The GPU host owns the CVDisplayLink idle pump, dispatched to the MAIN
    // queue, which dereferences the bridge. An AU v3 controller can be released
    // on the XPC connection queue (createAudioUnitWithComponentDescription and
    // the rebuild bounce arrive off-main), so destroying the host off-main would
    // race a queued main-queue idle block already past its liveness check →
    // use-after-free in display_link_callback (same class as the AU v2 fix).
    // Reset the host on the main thread FIRST — that flips its liveness token and
    // stops the display link — making teardown and the idle block mutually
    // exclusive on one queue. This preserves the destruction order below:
    // ivars destroy in reverse declaration order after [super dealloc]:
    // 1. ~PluginViewHost runs first, clears back-pointer on root_ View —
    //    already done here (the reset), so its later ivar destruction is a no-op.
    // 2. ~unique_ptr<View> (_fallbackView) — no-op on success path.
    // 3. ~ViewBridge — close() fires on_view_closed, releases scripted UI.
    // Explicitly closing the bridge HERE would reverse that order and
    // dereference a dangling root_ from ~PluginViewHost. Don't.
    if ([NSThread isMainThread]) {
        _viewHost.reset();
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{ _viewHost.reset(); });
    }
    [_audioUnit release];
    [super dealloc];
}
#endif

@end

#endif // TARGET_OS_OSX
#endif // __APPLE__
