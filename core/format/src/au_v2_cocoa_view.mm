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

#include <cmath>
#include <cstdlib>
#include <memory>

#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/format/detail/au_v2_editor_resize.hpp>
#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/gpu_host_select.hpp>
#include <pulp/format/editor_idle_pump.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_version.hpp>
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
    pulp::format::Processor* processor = nullptr;
    pulp::runtime::AliveToken::Handle processor_alive;
    const void* resize_owner = nullptr;
    // Forwards `host`'s GPU-surface transitions into `bridge`'s scripted UI
    // session. Declared LAST so it destroys FIRST — before both the host that
    // publishes and the bridge that is written into.
    pulp::view::PluginViewHost::GpuSurfaceSubscription gpu_surface_binding;

    ~PulpAUEditorOwnership() {
        // Remove the owner-scoped handler before host/bridge member teardown.
        // The adapter can outlive the Cocoa view, but the callback captures
        // both objects and the returned NSView.
        if (processor && resize_owner &&
            pulp::runtime::AliveToken::is_alive(processor_alive)) {
            processor->set_editor_resize_handler(resize_owner, nullptr);
        }
        // Explicit, not just reverse-member-order luck: the host publishes
        // `unavailable` from its own destructor.
        gpu_surface_binding.reset();
    }
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
        // idle pump (make_editor_idle_pump) is dispatched to the MAIN queue
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

namespace {

bool pulp_auv2_resize_trace_enabled() {
    const char* value = std::getenv("PULP_AUV2_RESIZE_TRACE");
    return value && value[0] != '\0' && value[0] != '0';
}

void pulp_trace_auv2_resize_hierarchy(NSView* editor_view, const char* phase) {
    if (!pulp_auv2_resize_trace_enabled()) return;

    NSLog(@"[pulp-auv2-resize] %s editor=%@ window=%@ window-frame=%@ content=%@",
          phase, editor_view, [editor_view window],
          NSStringFromRect([[editor_view window] frame]),
          [[editor_view window] contentView]);
    NSUInteger depth = 0;
    for (NSView* view = editor_view; view; view = [view superview], ++depth) {
        NSLog(@"[pulp-auv2-resize] %s ancestor[%lu] class=%@ frame=%@ bounds=%@ "
               "autoresizing=%lu translates-mask=%d",
              phase, static_cast<unsigned long>(depth),
              NSStringFromClass([view class]), NSStringFromRect([view frame]),
              NSStringFromRect([view bounds]),
              static_cast<unsigned long>([view autoresizingMask]),
              [view translatesAutoresizingMaskIntoConstraints]);
    }
}

bool pulp_resize_logic_auv2_editor(NSView* editor_view,
                                   pulp::view::PluginViewHost& editor_host,
                                   uint32_t next_w, uint32_t next_h) {
    if (![NSThread isMainThread] || !editor_view) return false;

    NSView* container = [editor_view superview];
    if (!container || ![editor_view window]) return false;

    const NSRect previous_view = [editor_view frame];
    const NSRect previous_container = [container frame];
    const NSSize requested = NSMakeSize(next_w, next_h);

    pulp_trace_auv2_resize_hierarchy(editor_view, "before");
    // Logic observes the returned editor view and propagates its exact size to
    // the immediate container and outer plug-in window. Mutating the container
    // first applies the same delta twice through its flexible autoresizing mask,
    // producing the alternating min/huge geometry seen during a live drag.
    // Never resize Logic's enclosing NSWindow directly either: the host owns its
    // chrome and mouse capture. One editor mutation is the complete transaction.
    editor_host.set_size(next_w, next_h);
    [editor_view setNeedsDisplay:YES];
    pulp_trace_auv2_resize_hierarchy(editor_view, "after");

    const NSRect applied_view = [editor_view frame];
    const NSRect applied_container = [container frame];
    const auto exact_size = [](NSSize actual, NSSize expected) {
        return std::fabs(actual.width - expected.width) < 0.5 &&
               std::fabs(actual.height - expected.height) < 0.5;
    };
    const auto exact_origin = [](NSPoint actual, NSPoint expected) {
        return std::fabs(actual.x - expected.x) < 0.5 &&
               std::fabs(actual.y - expected.y) < 0.5;
    };
    if (exact_size(applied_view.size, requested) &&
        exact_size(applied_container.size, requested) &&
        exact_origin(applied_view.origin, previous_view.origin) &&
        exact_origin(applied_container.origin, previous_container.origin)) {
        return true;
    }

    // Fail closed. A partial native resize is worse than a refusal because it
    // leaves the returned editor and its host container disagreeing about hit
    // coordinates.
    // Restore the host-owned parent first, then the returned editor last.
    // Reversing that order lets the parent's flexible autoresizing mask mutate
    // an already-restored child a second time.
    [container setFrame:previous_container];
    editor_host.set_size(
        static_cast<uint32_t>(std::lround(previous_view.size.width)),
        static_cast<uint32_t>(std::lround(previous_view.size.height)));
    [editor_view setFrameOrigin:previous_view.origin];
    pulp_trace_auv2_resize_hierarchy(editor_view, "rollback");
    return false;
}

}  // namespace

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
        *ctx.processor, *ctx.store, ctx.owner_alive,
        format::ViewBridge::Options::hosted_editor());
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

    // Viewport pin + aspect lock — parity with VST3 (PulpPlugView::attached)
    // and mac AUv3 (PulpAUViewController), via the shared
    // should_pin_design_viewport() predicate. Without this, a DAW resize of
    // the returned NSView (Logic resizes it directly; AU v2 has no size
    // negotiation) CLIPS the fixed-size tree instead of scaling it. The
    // free-drag case (resizable + aspect_ratio==0) stays unpinned — the root
    // reflows via Yoga through the frame-change forwarding below.
    // Top-align like mac AUv3: AU cannot negotiate the pane aspect (no
    // checkSizeConstraint / gui_adjust_size), so slack collects as a single
    // bottom strip instead of floating the content between two bands.
    const auto& hints = bridge->size_hints();
    format::configure_native_viewport(*host, hints);
    if (format::should_pin_design_viewport(hints)) {
        host->set_design_viewport_top_align(true);
    }

    // Run editor automation, restore/reload, and scripted work per vsync.
    // Captures the ViewBridge object
    // by address (stable across the unique_ptr move into the ownership wrapper
    // below); the wrapper destroys host (stops the display link) before bridge.
    host->set_idle_callback(format::make_editor_idle_pump(*bridge));

    // Follow the host's GpuSurface so JS navigator.gpu /
    // canvas.getContext('webgpu') routes through Pulp's Dawn instance, and so
    // the session drops the pointer when the host tears the surface down. Also
    // owns the CPU-fallback diagnostic. Moved into the ownership wrapper below.
    auto gpu_surface_binding = format::bind_gpu_surface(
        *host, bridge->scripted_ui(), gpu, "AU v2");

    // AU v2 has no host size callback — the DAW resizes the returned NSView
    // directly. Forward native frame changes to the bridge so the surfaces
    // resize and Processor::on_view_resized fires. A plugin-initiated resize
    // can synchronously bounce through the same NSView callback while Logic is
    // applying or rolling back its parent geometry; suppress those transient
    // frames and publish exactly once after an exact native acceptance.
    struct ResizeTransactionState {
        bool active = false;
    };
    auto resize_transaction = std::make_shared<ResizeTransactionState>();
    format::ViewBridge* bridge_ptr = bridge.get();
    host->set_resize_callback([bridge_ptr, resize_transaction](uint32_t w,
                                                               uint32_t h) {
        if (!resize_transaction->active) bridge_ptr->resize(w, h);
    });

    runtime::log_info("AU v2 editor: created view ({}x{}, mode={}, gpu={})",
                      w, h, gpu.mode, host->is_gpu_backed());

    NSView* editorView = (__bridge NSView*)host->native_handle();
    [editorView setFrame:NSMakeRect(0, 0, w, h)];

    // AU v2 plugin→host resize. Unlike AUv3 there is no
    // preferredContentSize/request-resize API: the Cocoa view's frame is the
    // plugin's editor size contract. Keep the transaction fail-closed if
    // AppKit clamps/refuses the requested frame, and mutate the design viewport
    // only after the exact native size sticks.
    format::ViewBridge* resize_bridge = bridge.get();
    const auto resize_bridge_alive = resize_bridge->alive_token();
    view::PluginViewHost* resize_host = host.get();
    __unsafe_unretained NSView* resize_view = editorView;
    const auto resize_host_info = format::detect_host_info();
    const bool resize_logic_container =
        format::resolved_quirks(resize_host_info.type, resize_host_info.version)
            .logic_au_v2_container_resize;
    if (pulp_auv2_resize_trace_enabled()) {
        NSLog(@"[pulp-auv2-resize] install host-type=%d host-version=%d.%d.%d "
               "logic-container=%d editor=%@ processor=%p",
              static_cast<int>(resize_host_info.type),
              resize_host_info.version.major, resize_host_info.version.minor,
              resize_host_info.version.patch,
              resize_logic_container, editorView,
              static_cast<void*>(ctx.processor));
    }
    format::au::editor_resize_detail::install_editor_resize_handler(
        *ctx.processor, (const void*)editorView, *resize_bridge,
        [resize_view, resize_bridge_alive, resize_logic_container,
         resize_transaction]() -> std::shared_ptr<void> {
            // request_editor_resize() copies its handler outside the
            // Processor side-table lock. Reject a stale copy before touching
            // any captured editor pointer, host, or bridge. AU v2 Cocoa editor
            // lifecycle and AppKit geometry are main-thread-only.
            if (![NSThread isMainThread] || !resize_logic_container ||
                resize_transaction->active ||
                !runtime::AliveToken::is_alive(resize_bridge_alive) ||
                !resize_view) {
                return {};
            }

            // Retain both the returned view and its associated ownership. The
            // latter owns the PluginViewHost + ViewBridge; retaining only the
            // NSView would not by itself make that C++ lifetime contract
            // explicit if a host removes or re-associates the view reentrantly.
            auto* owner = static_cast<PulpAUEditorOwner*>(
                objc_getAssociatedObject(resize_view, &kOwnershipKey));
            if (!owner) return {};
            [resize_view retain];
            [owner retain];

            struct LifetimeLease {
                NSView* view = nil;
                PulpAUEditorOwner* owner = nil;
                std::shared_ptr<ResizeTransactionState> transaction;
            };
            auto* lease = new LifetimeLease{
                resize_view, owner, resize_transaction};
            resize_transaction->active = true;
            return std::shared_ptr<void>(
                lease, [](void* opaque) {
                    auto* held = static_cast<LifetimeLease*>(opaque);
                    // Drop the in-flight guard before releasing ObjC lifetime;
                    // either release may synchronously run editor teardown.
                    held->transaction->active = false;
                    [held->view release];
                    [held->owner release];
                    delete held;
                });
        },
        [resize_view, resize_host,
         resize_logic_container](uint32_t next_w, uint32_t next_h) -> bool {
            if (pulp_auv2_resize_trace_enabled()) {
                NSLog(@"[pulp-auv2-resize] callback main=%d editor=%@ "
                       "logic-container=%d requested=%ux%u",
                      [NSThread isMainThread], resize_view,
                      resize_logic_container, next_w, next_h);
            }
            if (![NSThread isMainThread] || !resize_view ||
                !resize_logic_container) {
                return false;
            }
            return pulp_resize_logic_auv2_editor(
                resize_view, *resize_host, next_w, next_h);
        },
        [resize_host, resize_bridge](uint32_t next_w, uint32_t next_h) {
            format::commit_editor_requested_viewport(
                *resize_host, resize_bridge->size_hints(), next_w, next_h);
            if (format::should_pin_design_viewport(
                    resize_bridge->size_hints())) {
                resize_host->set_design_viewport_top_align(true);
            }
            // PluginViewHost::set_size updates its cached native frame before
            // asking AppKit to resize, so that programmatic mutation does not
            // re-enter its ordinary native resize callback. Publish the
            // accepted dimensions explicitly so Processor::on_view_resized
            // and ViewBridge's live size stay synchronized with Logic. This is
            // the final bridge/host access: the callback may close the editor.
            resize_bridge->resize(next_w, next_h);
        },
        [resize_view, resize_bridge_alive, resize_logic_container] {
            if (![NSThread isMainThread] || !resize_logic_container ||
                !runtime::AliveToken::is_alive(resize_bridge_alive) ||
                !resize_view) {
                return false;
            }
            // Logic can retain the returned NSView after closing its editor.
            // Only a view still attached to both a hierarchy and a window is
            // an eligible target for a new plugin-initiated resize.
            return [resize_view superview] != nil && [resize_view window] != nil;
        });
    bridge->notify_attached();

    // Transfer C++ ownership to an ObjC wrapper attached to the NSView.
    // When the NSView is deallocated, the wrapper's dealloc closes the
    // bridge (fires Processor::on_view_closed) and frees the host.
    auto* ownership = new PulpAUEditorOwnership{
        std::move(bridge), std::move(host), ctx.processor, ctx.owner_alive,
        (const void*)editorView, std::move(gpu_surface_binding)};
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
