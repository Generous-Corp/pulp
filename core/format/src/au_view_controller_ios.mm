// iOS AUv3 view controller — provides a UIViewController for the AUv3 extension UI.
// On iOS, AUv3 extensions present their UI via a UIViewController subclass.
// This wraps the Pulp view system into the AUv3 hosting model and routes
// editor lifecycle through `pulp::format::ViewBridge` so custom views,
// `Processor::create_view`, and `on_view_*` callbacks work identically
// to VST3 / CLAP / AU v2 / AU v3 macOS.

#if TARGET_OS_IOS

#import <UIKit/UIKit.h>
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

/// AUViewController subclass that hosts a Pulp View tree inside an AUv3 extension.
/// Implements AUAudioUnitFactory: createAudioUnitWithComponentDescription:error:
/// instantiates PulpAudioUnit, then `rebuildEditorIfReady` opens a ViewBridge
/// against the same Processor + StateStore the audio render block runs against
/// and attaches a PluginViewHost to the UIKit hierarchy. Apple says the AU
/// and the view controller may load in either order — so the rebuild runs
/// from both viewDidLoad AND setAudioUnit:.
@interface PulpAUViewController : AUViewController <AUAudioUnitFactory>

@property (nonatomic, strong) AUAudioUnit *audioUnit;

@end

@implementation PulpAUViewController {
    // Declaration order is load-bearing — see -dealloc. _viewHost holds a
    // `View& root_`; it MUST be destroyed before whichever object owns that
    // View (the bridge's view, OR _fallbackView in the no-bridge preview
    // path). Declaring _viewHost LAST makes it destroy FIRST (reverse order),
    // which is safe for both paths.
    pulp::format::Processor *_resizeProcessor;
    std::unique_ptr<pulp::format::ViewBridge> _bridge;
    std::unique_ptr<pulp::view::View> _fallbackView;
    std::unique_ptr<pulp::view::PluginViewHost> _viewHost;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.view.backgroundColor = [UIColor blackColor];
    self.preferredContentSize = CGSizeMake(400, 300);

    [self rebuildEditorIfReady];
}

- (void)setAudioUnit:(AUAudioUnit *)audioUnit {
    if (_audioUnit == audioUnit) return;
    // Preserve the previous unit until its processor handler is removed on
    // main. The factory setter runs on the XPC queue, while all raw processor
    // and editor-host state below is main-thread-owned.
    AUAudioUnit *previousAudioUnit = _audioUnit;
#if !__has_feature(objc_arc)
    [previousAudioUnit retain];
    [_audioUnit release];
    _audioUnit = [audioUnit retain];
#else
    _audioUnit = audioUnit;
#endif
    // The factory method runs on the XPC connection queue; UIViewController
    // APIs (preferredContentSize, self.view) require main thread. See the
    // macOS controller for the full backstory — same bug bites iOS.
    void (^rebuild)(void) = ^{
        (void)previousAudioUnit;
        if (self->_resizeProcessor) {
            self->_resizeProcessor->set_editor_resize_handler(
                (const void *)self, nullptr);
            self->_resizeProcessor = nullptr;
        }
        [self rebuildEditorIfReady];
#if !__has_feature(objc_arc)
        [previousAudioUnit release];
#endif
    };
    if ([NSThread isMainThread]) {
        rebuild();
    } else {
        dispatch_async(dispatch_get_main_queue(), rebuild);
    }
}

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                    error:(NSError **)error {
    // AUAudioUnitFactory contract: the factory call is what instantiates the
    // AU. Returning self.audioUnit unconditionally (as this method previously
    // did) left the AU nil and the host opened a generic-view editor over a
    // dead AU. Apple's docs and the AUViewController sample both create the
    // AU here so the host receives a live unit before the editor is built.
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
    // HARD GUARD: UIViewController state (preferredContentSize, self.view,
    // PluginViewHost UIKit attach) is main-thread-only. The host calls
    // createAudioUnitWithComponentDescription on the XPC connection queue;
    // setAudioUnit: bounces to main but the compiler can inline through
    // the property setter override. See au_view_controller_mac.mm for the
    // crash that bit us on macOS — same bug applies on iOS hosts (AUM,
    // Cubasis, GarageBand iOS).
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self rebuildEditorIfReady];
        });
        return;
    }

    if (pulp::format::detail::editor_launch_blocked_by_environment()) {
        pulp::runtime::log_info("AU iOS editor: disabled in headless/CI/test environment");
        return;
    }

    if (!self.audioUnit) {
        // Factory hasn't been called yet — setAudioUnit: will retry.
        return;
    }
    if (!self.isViewLoaded) {
        // viewDidLoad will retry once the view is built.
        return;
    }

    // Drop the previous editor first, in destruction order (handler → host →
    // fallback → bridge) so we can rebuild against a fresh ViewBridge.
    if (_resizeProcessor) {
        _resizeProcessor->set_editor_resize_handler((const void *)self, nullptr);
        _resizeProcessor = nullptr;
    }
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
            [(PulpAudioUnit *)self.audioUnit pulpOwnerAlive],
            pulp::format::ViewBridge::Options{
                .enable_hot_reload = pulp::format::dev_editor_hot_reload_enabled(),
                .role = pulp::format::ViewRole::Editor});
        std::string err;
        if (!_bridge->open(&err)) {
            pulp::runtime::log_error("AU iOS: ViewBridge::open failed ({})", err);
            _bridge.reset();
        } else {
            root = _bridge->view();
            w = _bridge->size_hints().preferred_width;
            h = _bridge->size_hints().preferred_height;
        }
    }

    if (!root) {
        // No audioUnit yet (preview case) — fall back to an empty View.
        _fallbackView = std::make_unique<pulp::view::View>();
        root = _fallbackView.get();
    }

    self.preferredContentSize = CGSizeMake(w, h);

    // Auto-select the GPU host for scripted / GPU-backed editors via the shared
    // decision helper. The preview/fallback case stays on the default CPU host.
    pulp::view::PluginViewHost::Options opts;
    opts.size = {w, h};
    const char* mode = "fallback";
    if (_bridge) {
        const auto gpu = pulp::format::decide_gpu_host(*_bridge);
        opts.use_gpu = gpu.use_gpu;
        mode = gpu.mode;
        _viewHost = pulp::view::PluginViewHost::create(*root, opts);
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
                        "via ScriptedUiSession (iOS AUv3)");
                }
            }
        }
    } else {
        _viewHost = pulp::view::PluginViewHost::create(*root, opts);
    }

    if (!_viewHost) {
        pulp::runtime::log_error("AU iOS: PluginViewHost::create returned null");
        return;
    }

    // iOS AU editors FILL the host pane and let the scripted UI reflow
    // responsively — Pulp scenes are flex-based and adapt to any size. We
    // deliberately do NOT pin a fixed design viewport here: aspect-locked
    // letterboxing scaled the design uniformly and left dark bars on the
    // sides of a scene (e.g. the Three.js cube) that can fill any size, and
    // pushed header text to the pane edge. Laying the root out at the actual
    // pane bounds (resizeEditorToViewBounds drives set_size + bridge resize)
    // lets a responsive scene fill edge-to-edge. A fixed-design editor that
    // genuinely needs letterboxing can call set_design_viewport itself.

    _viewHost->attach_to_parent((__bridge void*)self.view);

    // AUv3 publishes a preferredContentSize rather than receiving an
    // accept/refuse callback. Keep iOS responsive (no design viewport/aspect
    // pin), but let an editor mode publish its new natural size. The host's
    // eventual pane bounds remain authoritative through viewDidLayoutSubviews.
    if (processor && _bridge) {
        _resizeProcessor = processor;
        __unsafe_unretained PulpAUViewController *controller = self;
        processor->set_editor_resize_handler(
            (const void *)self,
            [controller](uint32_t requested_width,
                         uint32_t requested_height) -> bool {
                if (requested_width == 0 || requested_height == 0 ||
                    ![NSThread isMainThread]) {
                    return false;
                }
                if (!controller->_bridge ||
                    !controller->_bridge->set_preferred_size(
                        requested_width, requested_height)) {
                    return false;
                }
                controller.preferredContentSize =
                    CGSizeMake(requested_width, requested_height);
                return true;
            });
    }
    // Notify only after this view has registered its owner-scoped handler.
    // on_view_opened() may synchronously restore a mode-specific natural size;
    // registering first prevents that request from being dropped or routed to
    // another already-open view.
    if (_bridge) _bridge->notify_attached();

    pulp::runtime::log_info("AU iOS: view controller loaded, {}x{}, mode={}, gpu={}",
                            opts.size.width, opts.size.height, mode,
                            _viewHost->is_gpu_backed());

    [self resizeEditorToViewBounds];
}

- (void)resizeEditorToViewBounds {
    if (!_viewHost) return;
    CGSize size = self.view.bounds.size;
    const uint32_t w = std::max(1u, static_cast<uint32_t>(size.width));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(size.height));
    _viewHost->set_size(w, h);
    if (_bridge) _bridge->resize(w, h);
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    [self resizeEditorToViewBounds];
}

- (void)viewWillTransitionToSize:(CGSize)size
       withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    // viewDidLayoutSubviews is still the authority on the final bounds; this
    // hook just lets the host see a sized editor during the rotation / split-
    // view transition rather than after it lands.
    if (!_viewHost) return;
    const uint32_t w = std::max(1u, static_cast<uint32_t>(size.width));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(size.height));
    _viewHost->set_size(w, h);
    if (_bridge) _bridge->resize(w, h);
}

- (void)dealloc {
    // Destruction-order contract:
    //
    // PulpAUViewController declares its ivars in order:
    //     _bridge       (ViewBridge — owns the View tree on the success path)
    //     _fallbackView (the View on the no-bridge preview path)
    //     _viewHost     (PluginViewHost — holds `View& root_`)
    //
    // After [super dealloc] the runtime destroys C++-typed ivars in
    // REVERSE declaration order: _viewHost, then _fallbackView, then
    // _bridge. That ordering is load-bearing:
    //   1. ~unique_ptr<PluginViewHost> runs FIRST. Its destructor calls
    //      `root_.set_plugin_view_host(nullptr)` (and, for the GPU host,
    //      `root_.set_frame_clock(nullptr)`). `root_` references either
    //      `_bridge->view_` or `_fallbackView` — BOTH are still alive at
    //      this point, so the back-pointer clear is safe on either path.
    //      (Previously _viewHost was declared before _fallbackView, so on
    //      the fallback path the View died first and ~PluginViewHost
    //      dereferenced a dangling `root_`.)
    //   2. ~unique_ptr<View> (_fallbackView) runs (no-op when bridge
    //      succeeded; on the fallback path the back-pointer was cleared
    //      in step 1).
    //   3. ~unique_ptr<ViewBridge> runs. Its destructor calls `close()`
    //      which fires `Processor::on_view_closed`, releases the scripted
    //      UI, and resets the View. The back-pointer was already cleared
    //      in step 1, so the View's own teardown can't reach a dead host.
    //
    // Calling `_bridge->close()` HERE explicitly (before [super dealloc])
    // reverses that order — the View dies first, then ~PluginViewHost
    // dereferences a dangling `root_`. Don't reintroduce the explicit close.
    //
    // The GPU host owns the CVDisplayLink idle pump, dispatched to the MAIN
    // queue, which dereferences the bridge. An AU v3 controller can be released
    // off the main thread (setAudioUnit / rebuild arrive on the XPC queue), so
    // destroying the host off-main would race a queued main-queue idle block →
    // use-after-free (same class as the AU v2 fix). Reset the host on the main
    // thread FIRST (flips its liveness token + stops the display link) so
    // teardown and the idle block are mutually exclusive on one queue; its later
    // reverse-order ivar destruction is then a no-op and the contract above holds.
    void (^teardownMainThreadState)(void) = ^{
        if (self->_resizeProcessor) {
            self->_resizeProcessor->set_editor_resize_handler(
                (const void *)self, nullptr);
            self->_resizeProcessor = nullptr;
        }
        self->_viewHost.reset();
    };
    if ([NSThread isMainThread]) {
        teardownMainThreadState();
    } else {
        dispatch_sync(dispatch_get_main_queue(), teardownMainThreadState);
    }
#if !__has_feature(objc_arc)
    [_audioUnit release];
    [super dealloc];
#endif
}

@end

#endif // TARGET_OS_IOS
