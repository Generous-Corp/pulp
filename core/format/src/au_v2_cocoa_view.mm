// AU v2 Cocoa View Factory
// Implements the AUCocoaUIBase informal protocol to provide a custom NSView
// editor for AU v2 plugins. Creates an AutoUi-generated view tree embedded
// via PluginViewHost.
//
// The host discovers this class via kAudioUnitProperty_CocoaUI.

#ifdef PULP_AU_GUI

#import <AudioUnit/AUCocoaUIView.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/view/auto_ui.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/runtime/log.hpp>

// ── Ownership wrapper ──────────────────────────────────────────────────
// Wraps C++ ownership objects in an ObjC class so they share the NSView's
// lifetime via an associated object.

struct PulpAUEditorOwnership {
    std::unique_ptr<pulp::view::View> root;
    std::unique_ptr<pulp::view::PluginViewHost> host;
    std::shared_ptr<pulp::state::StateStore> store;
    std::unique_ptr<pulp::format::Processor> processor;
};

@interface PulpAUEditorOwner : NSObject {
    PulpAUEditorOwnership* _ownership;
}
- (instancetype)initWithOwnership:(PulpAUEditorOwnership*)ownership;
@end

@implementation PulpAUEditorOwner
- (instancetype)initWithOwnership:(PulpAUEditorOwnership*)ownership {
    self = [super init];
    if (self) _ownership = ownership;
    return self;
}
- (void)dealloc {
    delete _ownership;
    [super dealloc];
}
@end

static const char kOwnershipKey = 0;

// ── Cocoa View Factory ─────────────────────────────────────────────────

@interface PulpAUCocoaViewFactory : NSObject <AUCocoaUIBase>
@end

@implementation PulpAUCocoaViewFactory

- (unsigned)interfaceVersion {
    return 0;
}

- (NSView *)uiViewForAudioUnit:(AudioUnit)inAU withSize:(NSSize)inPreferredSize {
    using namespace pulp;

    // Get the processor factory from the registry
    auto factory_fn = format::registered_factory();
    if (!factory_fn) {
        runtime::log_error("AU v2 editor: no registered processor factory");
        return nil;
    }

    // Create a processor to get editor size and build parameters
    auto processor = factory_fn();
    if (!processor || !processor->has_editor()) {
        runtime::log_error("AU v2 editor: processor has no editor");
        return nil;
    }

    // Create a StateStore and define parameters
    auto store = std::make_shared<state::StateStore>();
    processor->set_state_store(store.get());
    processor->define_parameters(*store);

    // Sync current parameter values from the AudioUnit
    auto params = store->all_params();
    for (const auto& param : params) {
        Float32 value = 0;
        AudioUnitGetParameter(inAU,
            static_cast<AudioUnitParameterID>(param.id),
            kAudioUnitScope_Global, 0, &value);
        store->set_value(param.id, value);
    }

    // Build AutoUi view tree
    auto root = view::AutoUi::build(*store);
    if (!root) {
        runtime::log_error("AU v2 editor: AutoUi::build() returned nullptr");
        return nil;
    }

    auto [w, h] = processor->editor_size();
    view::PluginViewHost::Options opts;
    opts.size = {w, h};
    opts.use_gpu = false;

    auto host = view::PluginViewHost::create(*root, opts);
    if (!host) {
        runtime::log_error("AU v2 editor: PluginViewHost::create() failed");
        return nil;
    }

    NSView* editorView = (__bridge NSView*)host->native_handle();
    [editorView setFrame:NSMakeRect(0, 0, w, h)];

    // Transfer C++ ownership to an ObjC wrapper attached to the NSView.
    // When the NSView is deallocated, the wrapper's dealloc frees the C++ objects.
    auto* ownership = new PulpAUEditorOwnership{
        std::move(root), std::move(host), std::move(store), std::move(processor)
    };
    PulpAUEditorOwner* owner = [[PulpAUEditorOwner alloc] initWithOwnership:ownership];
    objc_setAssociatedObject(editorView, &kOwnershipKey, owner,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [owner release];

    runtime::log_info("AU v2 editor: created view ({}x{})", w, h);
    return editorView;
}

@end

// ── C++ helper for property dispatch ───────────────────────────────────

namespace pulp::format::au {

bool fill_cocoa_view_info(void* outData) {
    auto* info = static_cast<AudioUnitCocoaViewInfo*>(outData);

    // Get the bundle containing the PulpAUCocoaViewFactory class
    Class factoryClass = NSClassFromString(@"PulpAUCocoaViewFactory");
    if (!factoryClass) return false;

    NSBundle* classBundle = [NSBundle bundleForClass:factoryClass];
    if (!classBundle) return false;

    CFBundleRef bundle = (__bridge CFBundleRef)classBundle;
    info->mCocoaAUViewBundleLocation = CFBundleCopyBundleURL(bundle);
    info->mCocoaAUViewClass[0] = CFStringCreateCopy(kCFAllocatorDefault,
        CFSTR("PulpAUCocoaViewFactory"));

    return true;
}

} // namespace pulp::format::au

#endif // PULP_AU_GUI
