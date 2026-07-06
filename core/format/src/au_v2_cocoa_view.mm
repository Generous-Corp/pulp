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

#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/runtime/log.hpp>

// Per-plugin-unique Cocoa view factory class name. ObjC class names are
// process-global, so a fixed name would collide when two Pulp AU components
// load into one host. PulpPluginFormats.cmake injects PULP_AU_COCOA_VIEW_CLASS
// = PulpAUCocoaViewFactory_<MFR>_<CODE> per *_AU target; the @interface, the
// @implementation, and the name returned in AudioUnitCocoaViewInfo all derive
// from it so the registered class and the advertised name always match.
#ifndef PULP_AU_COCOA_VIEW_CLASS
#define PULP_AU_COCOA_VIEW_CLASS PulpAUCocoaViewFactory
#endif
#define PULP_STRINGIFY_IMPL(x) #x
#define PULP_STRINGIFY(x) PULP_STRINGIFY_IMPL(x)
static const char* const kPulpAUCocoaViewClassName = PULP_STRINGIFY(PULP_AU_COCOA_VIEW_CLASS);

// PulpAUEditorOwner is an internal helper attached to the editor view (its name
// is never advertised to the host). Give it a per-plugin-unique runtime name —
// derived from the already-unique cocoa view class — so two Pulp AU plug-ins in
// one host don't register the same ObjC class (which warns and lets the
// first-loaded copy shadow the others).
#define PULP_AU_CONCAT_IMPL(a, b) a##b
#define PULP_AU_CONCAT(a, b) PULP_AU_CONCAT_IMPL(a, b)
#define PulpAUEditorOwner PULP_AU_CONCAT(PulpAUEditorOwner_, PULP_AU_COCOA_VIEW_CLASS)

// ── Ownership wrapper ──────────────────────────────────────────────────
// Wraps C++ ownership objects in an ObjC class so they share the NSView's
// lifetime via an associated object.
//
// IMPORTANT: we no longer own a Processor / StateStore here — those are
// fetched from the host's PulpAUEffect via the private
// `kPulpEditorContextProperty`. Creating a second Processor for the
// view (the pre-ViewBridge behavior) silently desynchronized parameter
// state between the audio thread and the UI; the live adapter owns both.

struct PulpAUEditorOwnership {
    std::unique_ptr<pulp::format::ViewBridge> bridge;
    std::unique_ptr<pulp::view::PluginViewHost> host;
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
    if (_ownership) {
        // Destruction-order contract:
        // PulpAUEditorOwnership declares `bridge` first, then `host`.
        // C++ destroys members in REVERSE declaration order, so:
        //   1. ~unique_ptr<PluginViewHost> runs first. Its destructor
        //      calls `root_.set_plugin_view_host(nullptr)` — the View
        //      that `root_` references is still alive at this point
        //      (still owned by `bridge->view_`), so the call is safe
        //      and clears the back-pointer.
        //   2. ~unique_ptr<ViewBridge> runs second. Its destructor
        //      calls `close()` which fires `Processor::on_view_closed`,
        //      releases the scripted UI, and resets the View. The
        //      back-pointer was already cleared in step 1, so the
        //      View's own teardown can't reach a dead host.
        //
        // Calling `bridge->close()` HERE explicitly (before `delete`)
        // reverses that ordering — the View dies BEFORE the host, and
        // the host's destructor then dereferences a dangling `root_`
        // reference, crashing AU v2 editor close. Don't reintroduce
        // the explicit close in this dealloc path.
        //
        // Teardown MUST run on the main thread. The GPU host's CVDisplayLink
        // idle pump (make_scripted_idle_pump) is dispatched to the MAIN queue
        // and dereferences the bridge. If Logic's AU XPC tears the view down on
        // a background thread, destroying host+bridge here races a main-queue
        // idle block already past its liveness check → the pump touches a freed
        // bridge (SIGSEGV/PAC fault in display_link_callback). Serializing the
        // delete onto the main thread makes teardown and the idle block mutually
        // exclusive (both on the main queue), closing that race.
        PulpAUEditorOwnership* owned = _ownership;
        _ownership = nullptr;
        if ([NSThread isMainThread]) {
            delete owned;
        } else {
            dispatch_sync(dispatch_get_main_queue(), ^{ delete owned; });
        }
    }
    [super dealloc];
}
@end

static const char kOwnershipKey = 0;

// ── Cocoa View Factory ─────────────────────────────────────────────────

@interface PULP_AU_COCOA_VIEW_CLASS : NSObject <AUCocoaUIBase>
@end

@implementation PULP_AU_COCOA_VIEW_CLASS

- (unsigned)interfaceVersion {
    return 0;
}

- (NSView *)uiViewForAudioUnit:(AudioUnit)inAU withSize:(NSSize)inPreferredSize {
    using namespace pulp;

    // Fetch the host's Processor + StateStore via a private AU property.
    // This is the fix for the former dual-Processor bug — previously we
    // called `registered_factory()` here to spin up a second Processor
    // instance whose parameters drifted from the audio-thread Processor's.
    format::au::PulpEditorContext ctx{};
    UInt32 size = sizeof(ctx);
    OSStatus status = AudioUnitGetProperty(
        inAU,
        format::au::kPulpEditorContextProperty,
        kAudioUnitScope_Global, 0, &ctx, &size);
    if (status != noErr || !ctx.processor || !ctx.store) {
        runtime::log_error("AU v2 editor: could not fetch editor context (status={})",
                           static_cast<int>(status));
        return nil;
    }

    if (!ctx.processor->has_editor()) {
        runtime::log_error("AU v2 editor: processor has no editor");
        return nil;
    }
    if (pulp::format::detail::editor_launch_blocked_by_environment()) {
        runtime::log_info("AU v2 editor: disabled in headless/CI/test environment");
        return nil;
    }

    auto bridge = std::make_unique<format::ViewBridge>(
        *ctx.processor, *ctx.store,
        format::ViewBridge::Options{
            .enable_hot_reload = format::dev_editor_hot_reload_enabled(),
            .role = format::ViewRole::Editor});
    std::string editor_error;
    if (!bridge->open(&editor_error)) {
        runtime::log_error("AU v2 editor: ViewBridge::open failed ({})", editor_error);
        return nil;
    }

    const uint32_t w = bridge->size_hints().preferred_width;
    const uint32_t h = bridge->size_hints().preferred_height;

    const auto gpu = format::decide_gpu_host(*bridge);
    view::PluginViewHost::Options opts;
    opts.size = {w, h};
    opts.use_gpu = gpu.use_gpu;

    auto host = view::PluginViewHost::create(*bridge->view(), opts);
    if (!host) {
        runtime::log_error("AU v2 editor: PluginViewHost::create() failed");
        bridge->close();
        return nil;
    }
    format::warn_if_unexpected_cpu_fallback(gpu, host.get());

    // Pump the scripted UI session per vsync. Captures the ViewBridge object
    // by address (stable across the unique_ptr move into the ownership wrapper
    // below); the wrapper destroys host (stops the display link) before bridge.
    host->set_idle_callback(format::make_scripted_idle_pump(*bridge));

    // Route navigator.gpu / canvas.getContext('webgpu') through the host's
    // live GpuSurface.
    if (auto* scripted = bridge->scripted_ui()) {
        scripted->attach_gpu_surface(host->gpu_surface());
        if (host->gpu_surface()) {
            runtime::log_info(
                "[plugin-gpu-host] GpuSurface attached to WidgetBridge "
                "via ScriptedUiSession (AU v2)");
        }
    }

    // AU v2 has no host size callback — the DAW resizes the returned NSView
    // directly. Forward native frame changes to the bridge so the surfaces
    // resize and Processor::on_view_resized fires.
    format::ViewBridge* bridge_ptr = bridge.get();
    host->set_resize_callback([bridge_ptr](uint32_t w, uint32_t h) {
        bridge_ptr->resize(w, h);
    });

    bridge->notify_attached();

    runtime::log_info("AU v2 editor: created view ({}x{}, mode={}, gpu={})",
                      w, h, gpu.mode, host->is_gpu_backed());

    NSView* editorView = (__bridge NSView*)host->native_handle();
    [editorView setFrame:NSMakeRect(0, 0, w, h)];

    // Transfer C++ ownership to an ObjC wrapper attached to the NSView.
    // When the NSView is deallocated, the wrapper's dealloc closes the
    // bridge (fires Processor::on_view_closed) and frees the host.
    auto* ownership = new PulpAUEditorOwnership{std::move(bridge), std::move(host)};
    PulpAUEditorOwner* owner = [[PulpAUEditorOwner alloc] initWithOwnership:ownership];
    objc_setAssociatedObject(editorView, &kOwnershipKey, owner,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [owner release];

    return editorView;
}

@end

// ── C++ helper for property dispatch ───────────────────────────────────

namespace pulp::format::au {

bool fill_cocoa_view_info(void* outData) {
    @autoreleasepool {
        if (!outData) return false;
        auto* info = static_cast<AudioUnitCocoaViewInfo*>(outData);

        // Get the bundle containing the (per-plugin-unique) factory class.
        Class factoryClass = [PULP_AU_COCOA_VIEW_CLASS class];
        if (!factoryClass) return false;

        NSBundle* classBundle = [NSBundle bundleForClass:factoryClass];
        if (!classBundle) return false;

        // Get the bundle URL via NSBundle's ObjC accessor, NOT
        // CFBundleCopyBundleURL. The raw CFBundle path runs
        // __CFCheckCFInfoPACSignature, which raises a PAC_EXCEPTION /
        // SIGKILL in pointer-authentication-hardened, sandboxed hosts
        // (Logic Pro's AUHostingServiceXPC, and auval). That SIGKILL is a
        // hardware trap, NOT an NSException, so a @try can't catch it — it
        // takes down the whole host process. `-[NSBundle bundleURL]` returns
        // the same URL without the PAC-sensitive CFBundle access. This was
        // the actual crash that kept the Pulp AU editor from ever loading.
        @try {
            NSURL* bundleURL = [classBundle bundleURL];
            if (!bundleURL) return false;

            // The class name advertised here MUST equal the @implementation's
            // class (both derive from PULP_AU_COCOA_VIEW_CLASS) or the host
            // can't NSClassFromString() it. Ownership: AU CF view-info
            // properties are returned to the host, which releases them — so
            // hand over +1-retained CF objects.
            CFStringRef className = CFStringCreateWithCString(
                kCFAllocatorDefault, kPulpAUCocoaViewClassName, kCFStringEncodingUTF8);
            if (!className) return false;

            info->mCocoaAUViewBundleLocation = (CFURLRef)CFBridgingRetain(bundleURL);
            info->mCocoaAUViewClass[0] = className;
            return true;
        } @catch (NSException*) {
            return false;
        }
    }
}

// Install the filler into the shared adapter hook at image load. Only *_AU
// targets compile this TU (PULP_AU_GUI), so non-GUI builds of pulp-format
// leave g_cocoa_view_info_filler null and report no Cocoa view.
namespace {
struct CocoaViewInfoFillerRegistration {
    CocoaViewInfoFillerRegistration() { g_cocoa_view_info_filler = &fill_cocoa_view_info; }
    ~CocoaViewInfoFillerRegistration() {
        if (g_cocoa_view_info_filler == &fill_cocoa_view_info)
            g_cocoa_view_info_filler = nullptr;
    }
};
CocoaViewInfoFillerRegistration g_register_cocoa_view_info_filler;
} // namespace

} // namespace pulp::format::au

#endif // PULP_AU_GUI
