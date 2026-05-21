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
    std::unique_ptr<pulp::format::ViewBridge> _bridge;
    std::unique_ptr<pulp::view::PluginViewHost> _viewHost;
    std::unique_ptr<pulp::view::View> _fallbackView;
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
    auto size = pulp::view::PluginViewHost::Size{w, h};
    _viewHost = pulp::view::PluginViewHost::create(*root, size);

    if (_viewHost) {
        _viewHost->attach_to_parent((__bridge void*)self.view);
        if (_bridge) _bridge->notify_attached();
        pulp::runtime::log_info("AU iOS: view controller loaded, {}x{}, mode={}",
                                size.width, size.height,
                                _bridge ? (_bridge->uses_script_ui() ? "scripted" : "autoui")
                                        : "fallback");
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
    // Destruction-order contract (Codex P1 review on PR #653):
    //
    // PulpAUViewController declares its ivars in order:
    //     _bridge       (ViewBridge — owns the View tree)
    //     _viewHost     (PluginViewHost — holds `View& root_`)
    //     _fallbackView (only used when bridge fails)
    //
    // After [super dealloc] the runtime destroys C++-typed ivars in
    // REVERSE declaration order: _fallbackView, then _viewHost, then
    // _bridge. That ordering is load-bearing:
    //   1. ~_fallbackView runs (no-op when bridge succeeded).
    //   2. ~unique_ptr<PluginViewHost> runs. Its destructor calls
    //      `root_.set_plugin_view_host(nullptr)` — the View that
    //      `root_` references is still alive (still owned by
    //      `_bridge->view_`), so the call is safe and clears the
    //      back-pointer.
    //   3. ~unique_ptr<ViewBridge> runs. Its destructor calls
    //      `close()` which fires `Processor::on_view_closed`,
    //      releases the scripted UI, and resets the View. The
    //      back-pointer was already cleared in step 2, so the
    //      View's own teardown can't reach a dead host.
    //
    // Calling `_bridge->close()` HERE explicitly (before [super
    // dealloc]) reverses that order — the View dies first, then
    // ~PluginViewHost dereferences a dangling `root_` reference and
    // crashes AUv3 editor close. Don't reintroduce the explicit
    // close in this dealloc path.
    [super dealloc];
}

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                    error:(NSError **)error {
    return self.audioUnit;
}

@end

#endif // TARGET_OS_IOS
