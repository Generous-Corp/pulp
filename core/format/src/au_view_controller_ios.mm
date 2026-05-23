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

#include "pulp_audio_unit_fwd.h"

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
#if !__has_feature(objc_arc)
    [_audioUnit release];
    _audioUnit = [audioUnit retain];
#else
    _audioUnit = audioUnit;
#endif
    // The factory method runs on the XPC connection queue; UIViewController
    // APIs (preferredContentSize, self.view) require main thread. See the
    // macOS controller for the full backstory — same bug bites iOS.
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
    // AUAudioUnitFactory contract: the factory call is what instantiates the
    // AU. Returning self.audioUnit unconditionally (as this method previously
    // did) left the AU nil and the host opened a generic-view editor over a
    // dead AU. Apple's docs and the AUViewController sample both create the
    // AU here. See planning/2026-05-23-auv3-macos-ui-phase35.md
    // "P0: AudioUnit instance ownership".
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

    // Auto-select the GPU host for a scripted / GPU-backed editor (P5) via the
    // shared decision helper, using the Options overload. The preview/fallback
    // case (no bridge) stays on the default CPU host.
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
        }
    } else {
        _viewHost = pulp::view::PluginViewHost::create(*root, opts);
    }

    if (!_viewHost) {
        pulp::runtime::log_error("AU iOS: PluginViewHost::create returned null");
        return;
    }

    // Phase 3 viewport pin + aspect lock — paint at design size; host owns
    // window size; PluginViewHost scales the paint/input transform.
    if (w > 0 && h > 0) {
        _viewHost->set_design_viewport(w, h);
        _viewHost->set_fixed_aspect_ratio(static_cast<float>(w) /
                                          static_cast<float>(h));
    }

    _viewHost->attach_to_parent((__bridge void*)self.view);
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
    // Destruction-order contract (Codex P1 review on PR #653; fallback-path
    // UAF fix, GPU-plugin-view-host):
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
#if !__has_feature(objc_arc)
    [_audioUnit release];
    [super dealloc];
#endif
}

@end

#endif // TARGET_OS_IOS
