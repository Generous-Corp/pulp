// iOS AUv3 view controller — provides a UIViewController for the AUv3 extension UI.
// On iOS, AUv3 extensions present their UI via a UIViewController subclass.
// This wraps the Pulp view system into the AUv3 hosting model and routes
// editor lifecycle through `pulp::format::ViewBridge` so custom views,
// `Processor::create_view`, and `on_view_*` callbacks work identically
// to VST3 / CLAP / AU v2.

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

// Forward-declared selectors on PulpAudioUnit (implemented in au_adapter.mm).
@interface NSObject (PulpAUIntrospection)
- (pulp::format::Processor *)pulpProcessor;
- (pulp::state::StateStore *)pulpStore;
@end

/// AUViewController subclass that hosts a Pulp View tree inside an AUv3 extension.
/// When `self.audioUnit` is set, the controller fetches its Processor +
/// StateStore, builds a ViewBridge against them, and attaches the
/// resulting view tree to the UIKit hierarchy. Dealloc closes the bridge
/// so `Processor::on_view_closed` fires exactly once.
@interface PulpAUViewController : AUViewController

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

    if (pulp::format::detail::editor_launch_blocked_by_environment()) {
        pulp::runtime::log_info("AU iOS editor: disabled in headless/CI/test environment");
        return;
    }

    pulp::format::Processor *processor = nil;
    pulp::state::StateStore *store = nil;
    if ([self.audioUnit respondsToSelector:@selector(pulpProcessor)] &&
        [self.audioUnit respondsToSelector:@selector(pulpStore)]) {
        processor = [self.audioUnit pulpProcessor];
        store = [self.audioUnit pulpStore];
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

    if (_viewHost) {
        _viewHost->attach_to_parent((__bridge void*)self.view);
        if (_bridge) _bridge->notify_attached();
        pulp::runtime::log_info("AU iOS: view controller loaded, {}x{}, mode={}, gpu={}",
                                opts.size.width, opts.size.height, mode,
                                _viewHost->is_gpu_backed());
    }
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    if (_viewHost) {
        CGSize size = self.view.bounds.size;
        const uint32_t w = static_cast<uint32_t>(size.width);
        const uint32_t h = static_cast<uint32_t>(size.height);
        _viewHost->set_size(w, h);
        if (_bridge) _bridge->resize(w, h);
    }
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
    [super dealloc];
}

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                    error:(NSError **)error {
    return self.audioUnit;
}

@end

#endif // TARGET_OS_IOS
