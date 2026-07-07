// iOS plugin view host — UIView-based embedding for AUv3 in iOS hosts
// Mirrors plugin_view_host_mac.mm but uses UIKit instead of AppKit.

#include <pulp/view/plugin_view_host.hpp>

#if TARGET_OS_IOS

#include <pulp/canvas/cg_canvas.hpp>
#include <pulp/view/drag_drop.hpp>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

// ── PulpIOSDragDrop: UIDrop/UIDrag interaction delegate ──────────────────────
//
// Bridges UIKit's interaction-based drag-and-drop to Pulp's cross-platform
// dispatch core (the same dispatch_drag_*/dispatch_drop used by the mac, win,
// and linux hosts). INBOUND: a UIDropInteraction routes dropped file URLs into
// dispatch_drop() at the drop point. OUTBOUND: a UIDragInteraction starts a
// system drag carrying the files staged by PluginViewHost::start_file_drag().
//
// Paradigm note: UIKit drags are GESTURE-initiated (the system long-press lift
// asks the delegate for items), so unlike AppKit's synchronous
// NSDraggingSession, start_file_drag() cannot begin a drag itself — it ARMS the
// payload that the next lift consumes. hostView + pointTransform convert a
// UIView-space point to Pulp root space: identity for the CPU host, the inverse
// design-viewport map for the GPU (Metal) host.
@interface PulpIOSDragDrop : NSObject <UIDropInteractionDelegate, UIDragInteractionDelegate>
- (instancetype)initWithRoot:(pulp::view::View*)root
                    hostView:(UIView*)hostView
              pointTransform:(pulp::view::Point (^)(pulp::view::Point))xform;
// Arm an outbound drag with these absolute file paths; consumed by the next
// UIKit drag lift. Returns YES if a non-empty payload was staged.
- (BOOL)armOutboundDrag:(const std::vector<std::string>&)paths;
// Drop the root pointer when the host tears down, so an in-flight async drop
// completion never dereferences a destroyed view tree.
- (void)invalidate;
@end

@implementation PulpIOSDragDrop {
    pulp::view::View* _root;                       // not owned
    UIView* _hostView;                             // not owned; host invalidates before teardown
    pulp::view::Point (^_xform)(pulp::view::Point);
    pulp::view::DragSession _session;              // hover state for dispatch_drag_*
    NSMutableArray<NSString*>* _pendingPaths;      // staged outbound payload
    NSTimeInterval _armTime;                       // when _pendingPaths was staged
}

- (instancetype)initWithRoot:(pulp::view::View*)root
                    hostView:(UIView*)hostView
              pointTransform:(pulp::view::Point (^)(pulp::view::Point))xform {
    if (self = [super init]) {
        _root = root;
        _hostView = hostView;
        _xform = [xform copy];
        _pendingPaths = [[NSMutableArray alloc] init];
    }
    return self;
}

- (void)invalidate { _root = nullptr; }

- (void)dealloc {
    [_xform release];
    [_pendingPaths release];
    [super dealloc];
}

- (pulp::view::Point)rootPointFor:(id<UIDropSession>)session {
    CGPoint loc = [session locationInView:_hostView];
    pulp::view::Point pt{static_cast<float>(loc.x), static_cast<float>(loc.y)};
    if (_xform) pt = _xform(pt);
    return pt;
}

- (BOOL)armOutboundDrag:(const std::vector<std::string>&)paths {
    [_pendingPaths removeAllObjects];
    NSFileManager* fm = [NSFileManager defaultManager];
    for (const auto& p : paths) {
        if (p.empty()) continue;
        // Use the filesystem-representation inverse, not stringWithUTF8String:
        // (which returns nil for a non-UTF8 path and would crash addObject:).
        NSString* s = [fm stringWithFileSystemRepresentation:p.c_str() length:p.size()];
        if (s) [_pendingPaths addObject:s];
    }
    _armTime = [NSProcessInfo processInfo].systemUptime;
    return _pendingPaths.count > 0;
}

// ── UIDropInteractionDelegate (inbound) ──────────────────────────────────────

- (BOOL)dropInteraction:(UIDropInteraction *)interaction
       canHandleSession:(id<UIDropSession>)session {
    return [session canLoadObjectsOfClass:[NSURL class]];
}

- (UIDropProposal *)dropInteraction:(UIDropInteraction *)interaction
                   sessionDidUpdate:(id<UIDropSession>)session {
    // Only advertise Copy when a DropReceiver / on_drop actually accepts the
    // point — otherwise the whole view looks droppable and performDrop loads the
    // files just to discard them. dispatch_drag_move forwards to the idempotent
    // dispatch_drag_enter, which returns whether a receiver claimed the hover.
    BOOL accepted = NO;
    if (_root) {
        pulp::view::DropData data;
        data.type = pulp::view::DropData::Type::files;
        // dispatch_drag_enter is idempotent (safe to call on every update) and
        // returns whether a receiver claimed the hover.
        accepted = pulp::view::dispatch_drag_enter(*_root, _session, data,
                                                   [self rootPointFor:session]);
    }
    return [[UIDropProposal alloc]
        initWithDropOperation:accepted ? UIDropOperationCopy : UIDropOperationCancel];
}

- (void)dropInteraction:(UIDropInteraction *)interaction
         sessionDidExit:(id<UIDropSession>)session {
    if (_root) pulp::view::dispatch_drag_exit(*_root, _session);
}

- (void)dropInteraction:(UIDropInteraction *)interaction
            performDrop:(id<UIDropSession>)session {
    if (!_root) return;
    const pulp::view::Point pt = [self rootPointFor:session];
    PulpIOSDragDrop* retainedSelf = [self retain];
    // Completion is delivered on the main queue (UIKit guarantee), so touching
    // the view tree here is thread-safe.
    [session loadObjectsOfClass:[NSURL class]
                     completion:^(NSArray<__kindof id<NSItemProviderReading>> *objects) {
        PulpIOSDragDrop* strongSelf = retainedSelf;
        if (!strongSelf->_root) {
            [retainedSelf release];
            return;
        }
        pulp::view::DropData data;
        data.type = pulp::view::DropData::Type::files;
        for (id obj in objects) {
            if (![obj isKindOfClass:[NSURL class]]) continue;
            NSURL* url = (NSURL*)obj;
            if (!url.isFileURL) continue;
            if (const char* fs = url.fileSystemRepresentation)
                data.file_paths.emplace_back(fs);
        }
        if (!data.file_paths.empty())
            pulp::view::dispatch_drop(*strongSelf->_root, strongSelf->_session, data, pt);
        [retainedSelf release];
    }];
}

// ── UIDragInteractionDelegate (outbound) ─────────────────────────────────────

- (NSArray<UIDragItem *> *)dragInteraction:(UIDragInteraction *)interaction
                  itemsForBeginningSession:(id<UIDragSession>)session {
    // Expire a stale arm: start_file_drag stages paths for the NEXT lift, but if
    // that lift never comes (tap instead of drag, gesture cancelled), the payload
    // must not silently attach to a later unrelated drag. Bound the window so a
    // drag only carries freshly-armed files.
    if (_pendingPaths.count > 0 &&
        [NSProcessInfo processInfo].systemUptime - _armTime > 2.0) {
        [_pendingPaths removeAllObjects];
    }
    if (_pendingPaths.count == 0) return @[];  // nothing armed → no drag begins
    NSMutableArray<UIDragItem*>* items = [NSMutableArray array];
    for (NSString* path in _pendingPaths) {
        NSURL* url = [NSURL fileURLWithPath:path];
        NSItemProvider* provider = [[NSItemProvider alloc] initWithContentsOfURL:url];
        if (!provider) continue;
        [items addObject:[[UIDragItem alloc] initWithItemProvider:provider]];
    }
    [_pendingPaths removeAllObjects];  // consume the staged payload
    return items;
}

@end

// ── PulpPluginUIView: UIView subclass for DAW embedding on iOS ──────────────

@interface PulpPluginUIView : UIView {
    std::unordered_map<void*, int> _touchIdMap;
    int _nextTouchId;
    CADisplayLink* _gestureDisplayLink;
}
@property (nonatomic, assign) pulp::view::View* rootView;
@end

@implementation PulpPluginUIView

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.backgroundColor = [UIColor blackColor];
        self.multipleTouchEnabled = YES;
        self.contentMode = UIViewContentModeRedraw;
        _nextTouchId = 0;
        _gestureDisplayLink = nil;
    }
    return self;
}

- (void)dealloc {
    [_gestureDisplayLink invalidate];
    _gestureDisplayLink = nil;
    [super dealloc];
}

- (int)stableIdForTouch:(UITouch*)touch {
    void* key = (__bridge void*)touch;
    auto it = _touchIdMap.find(key);
    if (it != _touchIdMap.end()) return it->second;
    int newId = _nextTouchId++;
    _touchIdMap[key] = newId;
    return newId;
}

- (void)removeTouchId:(UITouch*)touch {
    _touchIdMap.erase((__bridge void*)touch);
    if (_touchIdMap.empty()) _nextTouchId = 0;
}

- (pulp::view::MouseEvent)mouseEventFromTouch:(UITouch*)touch isDown:(BOOL)isDown {
    CGPoint loc = [touch locationInView:self];
    pulp::view::MouseEvent me;
    me.position = {static_cast<float>(loc.x), static_cast<float>(loc.y)};
    me.window_position = me.position;
    me.button = pulp::view::MouseButton::left;
    me.pointer_id = [self stableIdForTouch:touch];
    me.is_down = isDown;
    me.modifiers = 0x8000;

    if (touch.maximumPossibleForce > 0)
        me.pressure = static_cast<float>(touch.force / touch.maximumPossibleForce);
    if (touch.type == UITouchTypePencil) {
        me.pointer_type = pulp::view::PointerType::pen;
        me.altitude_angle = static_cast<float>(touch.altitudeAngle);
        me.azimuth_angle = static_cast<float>([touch azimuthAngleInView:self]);
    } else {
        me.pointer_type = pulp::view::PointerType::touch;
    }

    return me;
}

- (void)drawRect:(CGRect)rect {
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    if (!ctx) return;

    CGRect bounds = self.bounds;

    // UIKit has origin at top-left (same as Pulp), but Core Graphics
    // has origin at bottom-left. Flip the coordinate system.
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, bounds.size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    pulp::canvas::CoreGraphicsCanvas canvas(ctx,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    // Clear background
    canvas.set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
    canvas.fill_rect(0, 0,
        static_cast<float>(bounds.size.width),
        static_cast<float>(bounds.size.height));

    if (self.rootView) {
        self.rootView->set_bounds({0, 0,
            static_cast<float>(bounds.size.width),
            static_cast<float>(bounds.size.height)});
        self.rootView->layout_children();
        self.rootView->paint_all(canvas);
    }

    CGContextRestoreGState(ctx);
}

- (void)syncGestureDisplayLink {
    const bool wants_frames =
        self.rootView && self.rootView->has_time_driven_gestures();
    if (wants_frames) {
        if (_gestureDisplayLink) return;
        _gestureDisplayLink = [CADisplayLink displayLinkWithTarget:self
                                                           selector:@selector(gestureDisplayLinkTick:)];
        [_gestureDisplayLink addToRunLoop:[NSRunLoop mainRunLoop]
                                   forMode:NSRunLoopCommonModes];
        return;
    }
    if (_gestureDisplayLink) {
        [_gestureDisplayLink invalidate];
        _gestureDisplayLink = nil;
    }
}

- (void)gestureDisplayLinkTick:(CADisplayLink*)link {
    (void)link;
    if (!self.rootView) {
        [self syncGestureDisplayLink];
        return;
    }
    self.rootView->advance_gesture_recognizers();
    [self setNeedsDisplay];
    [self syncGestureDisplayLink];
}

// ── Touch handling → MouseEvent with stable pointer identity ────────────────

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:YES];
        me.phase = pulp::view::MousePhase::press;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_down(me.position);
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:YES];
        me.phase = pulp::view::MousePhase::drag;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_drag(me.position);
    }
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        me.phase = pulp::view::MousePhase::release;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self removeTouchId:touch];
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_up(me.position);
        [self removeTouchId:touch];
    }
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    if (!self.rootView) return;
    for (UITouch *touch in touches) {
        auto me = [self mouseEventFromTouch:touch isDown:NO];
        me.is_cancelled = true;
        me.phase = pulp::view::MousePhase::release;
        if (self.rootView->dispatch_gesture_pointer_event(me)) {
            [self removeTouchId:touch];
            [self syncGestureDisplayLink];
            [self setNeedsDisplay];
            continue;
        }
        self.rootView->on_mouse_cancel(me.position);
        [self removeTouchId:touch];
    }
}

// ── Safe area insets ────────────────────────────────────────────────────────

- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self setNeedsDisplay];
}

@end

static CGRect child_view_frame_in_host(UIView* container,
                                       float x,
                                       float y,
                                       float width,
                                       float height) {
    if (!container) {
        return CGRectZero;
    }

    const CGFloat clipped_width = std::max<CGFloat>(0.0, width);
    const CGFloat clipped_height = std::max<CGFloat>(0.0, height);
    return CGRectMake(x, y, clipped_width, clipped_height);
}

static bool attach_child_view_to_host(UIView* container,
                                      void* child_view_handle,
                                      float x,
                                      float y,
                                      float width,
                                      float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    UIView* child = (__bridge UIView*)child_view_handle;
    if (!child) {
        return false;
    }

    if (child.superview && child.superview != container) {
        [child removeFromSuperview];
    }

    child.frame = child_view_frame_in_host(container, x, y, width, height);
    if (child.superview != container) {
        [container addSubview:child];
    }
    child.hidden = NO;
    return true;
}

static bool set_child_view_bounds_in_host(UIView* container,
                                          void* child_view_handle,
                                          float x,
                                          float y,
                                          float width,
                                          float height) {
    if (!container || !child_view_handle) {
        return false;
    }

    UIView* child = (__bridge UIView*)child_view_handle;
    if (!child || child.superview != container) {
        return false;
    }

    child.frame = child_view_frame_in_host(container, x, y, width, height);
    return true;
}

static void detach_child_view_from_host(UIView* container, void* child_view_handle) {
    if (!container || !child_view_handle) {
        return;
    }

    UIView* child = (__bridge UIView*)child_view_handle;
    if (child && child.superview == container) {
        [child removeFromSuperview];
    }
}

// Mask an attached child view to a visible sub-rectangle expressed in the
// child's OWN top-left [0,0,frame_w,frame_h] box (Pulp convention), so a native
// child inside a scroll region is clipped to its scroll ancestor's viewport
// without resizing (and reflowing) the child. UIView layers are top-left
// origin, so the local clip maps directly. has_clip=false removes any mask.
static bool clip_child_view_in_host(UIView* container,
                                    void* child_view_handle,
                                    bool has_clip,
                                    float x,
                                    float y,
                                    float width,
                                    float height) {
    if (!container || !child_view_handle) {
        return false;
    }
    UIView* child = (__bridge UIView*)child_view_handle;
    if (!child || child.superview != container) {
        return false;
    }

    if (!has_clip) {
        child.layer.mask = nil;
        return true;
    }

    CALayer* mask = child.layer.mask;
    if (!mask) {
        mask = [CALayer layer];
        mask.backgroundColor = [UIColor blackColor].CGColor;  // opaque = visible
    }
    mask.frame = CGRectMake(x, y, std::max<CGFloat>(0.0, width),
                            std::max<CGFloat>(0.0, height));
    child.layer.mask = mask;
    return true;
}

// ── iOSPluginViewHost ─────────────────────────────────────────────────────────

namespace pulp::view {

class IOSPluginViewHost : public PluginViewHost {
public:
    IOSPluginViewHost(View& root, Size size)
        : root_(root), size_(size) {
        @autoreleasepool {
            CGRect frame = CGRectMake(0, 0, size.width, size.height);
            view_ = [[PulpPluginUIView alloc] initWithFrame:frame];
            view_.rootView = &root_;
            // Native drag-and-drop. The CPU view paints at logical 1:1, so a
            // drop point in view space IS root space — no transform needed.
            drag_drop_ = [[PulpIOSDragDrop alloc] initWithRoot:&root_
                                                      hostView:view_
                                                pointTransform:nil];
            [view_ addInteraction:[[UIDropInteraction alloc] initWithDelegate:drag_drop_]];
            UIDragInteraction* drag =
                [[UIDragInteraction alloc] initWithDelegate:drag_drop_];
            drag.enabled = YES;
            [view_ addInteraction:drag];
        }
    }

    ~IOSPluginViewHost() override {
        root_.set_plugin_view_host(nullptr);
        [drag_drop_ invalidate];
        detach();
    }

    // Arm an outbound file drag (drag audio out to Files / another app). On iOS
    // the UIKit drag begins from the user's lift gesture, so this stages the
    // payload for the next UIDragInteraction lift rather than starting a drag
    // synchronously (see PulpIOSDragDrop). Returns false if no files are given.
    bool start_file_drag(const FileDragRequest& request) override {
        if (request.file_paths.empty()) return false;
        return [drag_drop_ armOutboundDrag:request.file_paths] == YES;
    }

    NativeViewHandle native_handle() override {
        return (__bridge void*)view_;
    }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            UIView* parent_view = (__bridge UIView*)parent;
            if (parent_view && view_) {
                [parent_view addSubview:view_];
                [view_ setNeedsDisplay];
            }
        }
    }

    bool is_attached() const noexcept override {
        return view_ != nil && view_.superview != nil;
    }

    void detach() override {
        @autoreleasepool {
            if (view_) {
                [view_ removeFromSuperview];
            }
        }
    }

    void repaint() override {
        @autoreleasepool {
            [view_ setNeedsDisplay];
        }
    }

    void set_size(uint32_t width, uint32_t height) override {
        size_ = {width, height};
        @autoreleasepool {
            view_.frame = CGRectMake(0, 0, width, height);
            [view_ setNeedsDisplay];
        }
    }

    Size get_size() const override {
        return size_;
    }

    bool attach_native_child_view(NativeViewHandle child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        return attach_child_view_to_host(view_, child_view, x, y, width, height);
    }

    bool set_native_child_view_bounds(NativeViewHandle child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        return set_child_view_bounds_in_host(view_, child_view, x, y, width, height);
    }

    void detach_native_child_view(NativeViewHandle child_view) override {
        detach_child_view_from_host(view_, child_view);
    }

    bool set_native_child_view_clip(NativeViewHandle child_view,
                                    bool has_clip,
                                    float x,
                                    float y,
                                    float width,
                                    float height) override {
        return clip_child_view_in_host(view_, child_view, has_clip, x, y, width,
                                       height);
    }

private:
    View& root_;
    Size size_;
    PulpPluginUIView* view_ = nil;
    PulpIOSDragDrop* drag_drop_ = nil;
};

// ── GPU-accelerated iOS plugin view host ─────────────────────────────────

// Close the open `namespace pulp::view {` before pulling in Skia / Dawn /
// Metal headers — otherwise `pulp::render::*` ends up nested as
// `pulp::view::pulp::render::*` and Metal's Objective-C `@protocol`
// declarations land inside a C++ namespace, which the compiler rejects
// ("Objective-C declarations may only appear in global scope"). The
// bug is specific to GPU-enabled iOS configures because CPU-only iOS
// builds never include these headers.
}  // namespace pulp::view

#ifdef PULP_HAS_SKIA
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/window_host.hpp>  // WindowHost::compute_design_viewport_transform
#include <pulp/runtime/log.hpp>
#include <functional>
#include <memory>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

namespace pulp::view {

class IOSGpuPluginViewHost;

}  // namespace pulp::view

// Metal-backed UIView for GPU rendering in AUv3 hosts.
//
// Like the mac embedded view, the display link is driven by window-attach,
// not by attach_to_parent — the AUv3 controller adds this view to its own
// hierarchy, so the host wires `onWindowChange` / `onLayout` to start/stop
// the link and re-sync surfaces at the right times.
@interface PulpMetalPluginView : UIView
@property (nonatomic, copy) void (^onWindowChange)(void);
@property (nonatomic, copy) void (^onLayout)(void);
// View tree the host paints into — set by IOSGpuPluginViewHost so touch input
// can hit_test + dispatch JS pointer events (the GPU path previously had no
// touch handling at all, so a scripted editor like the Three.js cube was
// completely inert to drag/pinch).
@property (nonatomic, assign) pulp::view::View* rootView;
// Inverse design-viewport transform applied to each touch point before
// hit_test, mirroring the mac plugin host's pointTransform. nil = identity.
@property (nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
@end

namespace {

// Walk target→root accumulating each view's origin to convert a root-space
// point into target-local coords. Inverse of the local→window accumulation the
// widget bridge uses when it forwards drags, so offsetX/Y land where a widget
// expects them.
pulp::view::Point ios_root_to_local(pulp::view::Point root_pt, pulp::view::View* target) {
    float lx = root_pt.x, ly = root_pt.y;
    for (pulp::view::View* cur = target; cur; cur = cur->parent()) {
        const auto& b = cur->bounds();
        lx -= b.x;
        ly -= b.y;
    }
    return {lx, ly};
}

// Is `needle` still somewhere under `root`? Walks root DOWN comparing pointer
// identity — it must NEVER dereference `needle` itself, because a captured
// drag target can be destroyed (editor rebuild / JS unmount) between touch
// events. Walking needle→parent would deref the freed pointer; walking
// root→children only touches live nodes and compares addresses. Mirrors
// window_host_mac_geometry.mm's view_is_in_tree.
bool ios_view_in_tree(pulp::view::View* needle, pulp::view::View* root) {
    if (!needle || !root) return false;
    if (needle == root) return true;
    for (size_t i = 0; i < root->child_count(); ++i) {
        if (ios_view_in_tree(needle, root->child_at(i))) return true;
    }
    return false;
}

// Build a MouseEvent in ROOT space from a UITouch. `position` is overwritten
// with target-local coords by the caller; `window_position` stays root-space
// (the bridge maps it to clientX/Y, which web-compat mirrors into pageX/Y for
// OrbitControls' touch path).
pulp::view::MouseEvent ios_mouse_event_from_touch(
        UIView* view, UITouch* touch, int pointer_id, bool is_down,
        pulp::view::Point (^xform)(pulp::view::Point)) {
    CGPoint loc = [touch locationInView:view];
    pulp::view::Point pt{static_cast<float>(loc.x), static_cast<float>(loc.y)};
    if (xform) pt = xform(pt);  // host-space → root-space (inverse design viewport)
    pulp::view::MouseEvent me;
    me.position = pt;
    me.window_position = pt;
    me.button = pulp::view::MouseButton::left;
    me.pointer_id = pointer_id;
    me.is_down = is_down;
    me.modifiers = 0x8000;
    if (touch.maximumPossibleForce > 0)
        me.pressure = static_cast<float>(touch.force / touch.maximumPossibleForce);
    if (touch.type == UITouchTypePencil) {
        me.pointer_type = pulp::view::PointerType::pen;
        me.altitude_angle = static_cast<float>(touch.altitudeAngle);
        me.azimuth_angle = static_cast<float>([touch azimuthAngleInView:view]);
    } else {
        me.pointer_type = pulp::view::PointerType::touch;
    }
    return me;
}

}  // namespace

@implementation PulpMetalPluginView {
    // Stable per-UITouch pointer ids (OrbitControls keys its pinch dolly on two
    // distinct touch pointerIds, so the SAME finger must keep the SAME id across
    // moves). Same scheme as the CPU PulpPluginUIView.
    std::unordered_map<void*, int> _touchIdMap;
    int _nextTouchId;
    // Per-pointer drag target captured at touchesBegan and reused for that
    // pointer's moves/ends, so a finger that drifts off the canvas still routes
    // to the gesture owner (mirrors the mac plugin host's _dragTarget, but
    // per-pointer for multi-touch).
    std::unordered_map<int, pulp::view::View*> _dragTargets;
}
+ (Class)layerClass { return [CAMetalLayer class]; }
- (CAMetalLayer *)metalLayer { return (CAMetalLayer *)self.layer; }
- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _nextTouchId = 0;
        self.multipleTouchEnabled = YES;
    }
    return self;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.onWindowChange) self.onWindowChange();
}
- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.onLayout) self.onLayout();
}

- (int)stableIdForTouch:(UITouch*)touch {
    void* key = (__bridge void*)touch;
    auto it = _touchIdMap.find(key);
    if (it != _touchIdMap.end()) return it->second;
    int newId = _nextTouchId++;
    _touchIdMap[key] = newId;
    return newId;
}
- (void)removeTouchId:(UITouch*)touch {
    _touchIdMap.erase((__bridge void*)touch);
    if (_touchIdMap.empty()) _nextTouchId = 0;
}

// ── Touch → Pulp View hit_test + JS pointer/mouse dispatch ──────────────────
// Mirrors the mac plugin host's pulp_plugin_mouse_{down,drag,up}: hit_test on
// down, capture a per-pointer drag target, fire on_mouse_event (→ JS
// pointerdown/up) + on_pointer_move (→ JS pointermove with full identity), and
// bubble to ancestors that registered handlers. JS handlers reach into the
// bridge and CAN throw across this ObjC frame, so each dispatch is wrapped in
// try/catch exactly as the mac host does.
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    pulp::view::View* root = self.rootView;
    if (!root) return;
    for (UITouch* touch in touches) {
      try {
        const int pid = [self stableIdForTouch:touch];
        auto me = ios_mouse_event_from_touch(self, touch, pid, /*is_down=*/true,
                                             self.pointTransform);
        me.phase = pulp::view::MousePhase::press;
        if (root->dispatch_gesture_pointer_event(me)) continue;
        pulp::view::View* target = root->hit_test(me.window_position);
        if (!target) continue;
        _dragTargets[pid] = target;
        if (target->focusable()) target->claim_input_focus();
        pulp::view::MouseEvent local = me;
        local.position = ios_root_to_local(me.window_position, target);
        target->on_mouse_event(local);  // fires on_pointer_event → JS pointerdown
        // Also drive the native press virtual so View subclasses that init
        // drag state in on_mouse_down (knobs, faders) work in the GPU host —
        // mirrors mac's pulp_plugin_mouse_down.
        target->on_mouse_down(local.position);
        for (pulp::view::View* b = target->parent(); b; b = b->parent()) {
            if (!b->on_pointer_event) continue;
            pulp::view::MouseEvent bme = me;
            bme.position = ios_root_to_local(me.window_position, b);
            b->on_pointer_event(bme);
        }
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[plugin-gpu-host] touchesBegan handler threw: %s\n", e.what());
      } catch (...) {
        std::fprintf(stderr, "[plugin-gpu-host] touchesBegan handler threw (unknown)\n");
      }
    }
    root->request_repaint();
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    pulp::view::View* root = self.rootView;
    if (!root) return;
    for (UITouch* touch in touches) {
      try {
        const int pid = [self stableIdForTouch:touch];
        auto me = ios_mouse_event_from_touch(self, touch, pid, /*is_down=*/true,
                                             self.pointTransform);
        me.phase = pulp::view::MousePhase::drag;
        if (root->dispatch_gesture_pointer_event(me)) continue;
        auto it = _dragTargets.find(pid);
        if (it == _dragTargets.end()) continue;
        pulp::view::View* target = it->second;
        if (!ios_view_in_tree(target, root)) { _dragTargets.erase(it); continue; }
        me.position = ios_root_to_local(me.window_position, target);
        target->on_mouse_drag(me.position);  // legacy single-pointer native path
        // Identity-preserving pointermove (real pointerId + pointerType:'touch')
        // — this, not on_drag, is what lets OrbitControls track two fingers for
        // pinch-zoom. on_pointer_move also fans out the mouse equivalents, so we
        // deliberately do NOT also call on_drag (that would double-fire
        // pointermove under a phantom pointerId:0/mouse identity).
        if (target->on_pointer_move) target->on_pointer_move(me);
        for (pulp::view::View* b = target->parent(); b; b = b->parent()) {
            if (!b->on_pointer_move) continue;
            pulp::view::MouseEvent bme = me;
            bme.position = ios_root_to_local(me.window_position, b);
            b->on_pointer_move(bme);
        }
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[plugin-gpu-host] touchesMoved handler threw: %s\n", e.what());
      } catch (...) {
        std::fprintf(stderr, "[plugin-gpu-host] touchesMoved handler threw (unknown)\n");
      }
    }
    root->request_repaint();
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self finishTouches:touches cancelled:NO];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self finishTouches:touches cancelled:YES];
}

- (void)finishTouches:(NSSet<UITouch *> *)touches cancelled:(BOOL)cancelled {
    pulp::view::View* root = self.rootView;
    for (UITouch* touch in touches) {
      try {
        const int pid = [self stableIdForTouch:touch];
        auto it = _dragTargets.find(pid);
        pulp::view::View* target = (it != _dragTargets.end()) ? it->second : nullptr;
        if (root) {
            auto gesture_me = ios_mouse_event_from_touch(self, touch, pid, /*is_down=*/false,
                                                        self.pointTransform);
            gesture_me.is_cancelled = cancelled;
            gesture_me.phase = pulp::view::MousePhase::release;
            if (root->dispatch_gesture_pointer_event(gesture_me)) {
                if (it != _dragTargets.end()) _dragTargets.erase(it);
                [self removeTouchId:touch];
                continue;
            }
        }
        if (root && target && ios_view_in_tree(target, root)) {
            auto me = ios_mouse_event_from_touch(self, touch, pid, /*is_down=*/false,
                                                 self.pointTransform);
            me.is_cancelled = cancelled;
            me.phase = pulp::view::MousePhase::release;
            me.position = ios_root_to_local(me.window_position, target);
            // Distinct native cancel path so widgets can roll back in-progress
            // gestures instead of treating a cancel as a commit.
            if (cancelled) target->on_mouse_cancel(me.position);
            else target->on_mouse_up(me.position);
            target->on_mouse_event(me);  // fires on_pointer_event → JS pointerup/cancel
            for (pulp::view::View* b = target->parent(); b; b = b->parent()) {
                if (!b->on_pointer_event) continue;
                pulp::view::MouseEvent bme = me;
                bme.position = ios_root_to_local(me.window_position, b);
                b->on_pointer_event(bme);
            }
        }
        if (it != _dragTargets.end()) _dragTargets.erase(it);
        [self removeTouchId:touch];
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[plugin-gpu-host] touchesEnded handler threw: %s\n", e.what());
      } catch (...) {
        std::fprintf(stderr, "[plugin-gpu-host] touchesEnded handler threw (unknown)\n");
      }
    }
    if (root) root->request_repaint();
}
@end

// CADisplayLink target — relays the vsync tick to the C++ host.
@interface PulpIOSPluginDisplayLinkTarget : NSObject
@property (nonatomic, assign) pulp::view::IOSGpuPluginViewHost* host;
- (void)tick:(CADisplayLink*)link;
@end

namespace pulp::view {

class IOSGpuPluginViewHost : public PluginViewHost {
public:
    IOSGpuPluginViewHost(View& root, const Options& opts)
        : root_(root), size_(opts.size),
          alive_(std::make_shared<std::atomic<bool>>(true)) {
        @autoreleasepool {
            root_.set_frame_clock(&frame_clock_);
            CGRect frame = CGRectMake(0, 0, opts.size.width, opts.size.height);
            metal_view_ = [[PulpMetalPluginView alloc] initWithFrame:frame];
            metal_view_.multipleTouchEnabled = YES;
            metal_view_.rootView = &root_;  // enable touch → JS pointer dispatch
            metal_view_.autoresizingMask =
                UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            metal_view_.onWindowChange = ^{ this->handle_window_change(); };
            metal_view_.onLayout = ^{ this->handle_layout(); };

            // Native drag-and-drop. Map a drop point through the Metal view's
            // LIVE pointTransform (the inverse design-viewport map touches use),
            // captured weakly so a drop never resurrects a torn-down view.
            __weak PulpMetalPluginView* weak_view = metal_view_;
            drag_drop_ = [[PulpIOSDragDrop alloc] initWithRoot:&root_
                                                      hostView:metal_view_
                                                pointTransform:^pulp::view::Point(pulp::view::Point p) {
                PulpMetalPluginView* v = weak_view;
                return (v && v.pointTransform) ? v.pointTransform(p) : p;
            }];
            [metal_view_ addInteraction:[[UIDropInteraction alloc] initWithDelegate:drag_drop_]];
            UIDragInteraction* drag =
                [[UIDragInteraction alloc] initWithDelegate:drag_drop_];
            drag.enabled = YES;
            [metal_view_ addInteraction:drag];

            scale_ = current_scale();
            uint32_t pw = static_cast<uint32_t>(opts.size.width * scale_);
            uint32_t ph = static_cast<uint32_t>(opts.size.height * scale_);

            CAMetalLayer* layer = [metal_view_ metalLayer];
            layer.device = MTLCreateSystemDefaultDevice();
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = NO;
            layer.drawableSize = CGSizeMake(pw, ph);
            layer.contentsScale = scale_;

            // Current render API: create the Dawn surface, then build the
            // Skia Graphite surface from it (mirrors the mac host). GpuSurface
            // gets PHYSICAL pixels; SkiaSurface gets LOGICAL size + scale.
            gpu_surface_ = render::GpuSurface::create_dawn();
            if (gpu_surface_) {
                render::GpuSurface::Config gpu_cfg{};
                gpu_cfg.width = pw;
                gpu_cfg.height = ph;
                gpu_cfg.native_surface_handle = (__bridge void*)layer;
                if (gpu_surface_->initialize(gpu_cfg)) {
                    render::SkiaSurface::Config skia_cfg{};
                    skia_cfg.width = opts.size.width;
                    skia_cfg.height = opts.size.height;
                    skia_cfg.scale_factor = static_cast<float>(scale_);
                    skia_surface_ = render::SkiaSurface::create(*gpu_surface_, skia_cfg);
                    if (!skia_surface_) gpu_surface_.reset();
                } else {
                    gpu_surface_.reset();
                }
            }
        }
    }

    ~IOSGpuPluginViewHost() override {
        alive_->store(false, std::memory_order_release);
        root_.set_plugin_view_host(nullptr);
        stop_display_link();
        @autoreleasepool {
            // Clear the pointTransform block BEFORE this C++ object is freed —
            // it captures `this` raw, and a touch delivered to a retained view
            // after host disposal would otherwise call into freed memory
            // (mirrors the mac plugin host). Drop rootView for the same reason.
            metal_view_.pointTransform = nil;
            metal_view_.rootView = nullptr;
            metal_view_.onWindowChange = nil;
            metal_view_.onLayout = nil;
            [drag_drop_ invalidate];
        }
        skia_surface_.reset();
        gpu_surface_.reset();
        root_.set_frame_clock(nullptr);
    }

    NativeViewHandle native_handle() override { return (__bridge void*)metal_view_; }

    // Arm an outbound file drag — consumed by the next UIKit drag lift (UIKit
    // drags are gesture-initiated; see PulpIOSDragDrop). Returns false if empty.
    bool start_file_drag(const FileDragRequest& request) override {
        if (request.file_paths.empty()) return false;
        return [drag_drop_ armOutboundDrag:request.file_paths] == YES;
    }

    void attach_to_parent(NativeViewHandle parent) override {
        @autoreleasepool {
            UIView* pv = (__bridge UIView*)parent;
            if (pv && metal_view_) {
                [pv addSubview:metal_view_];
                // Display link starts via -didMoveToWindow → handle_window_change().
                needs_repaint_.store(true, std::memory_order_relaxed);
            }
        }
    }

    bool is_attached() const noexcept override {
        return metal_view_ != nil && metal_view_.superview != nil;
    }

    void detach() override {
        @autoreleasepool {
            stop_display_link();
            [metal_view_ removeFromSuperview];
        }
    }

    void repaint() override {
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    void tick() {
        if (!alive_->load(std::memory_order_acquire)) return;
        // Pump idle (scripted poll: async results, timers, rAF) FIRST so any
        // request_repaint they trigger is seen below.
        if (idle_callback_) idle_callback_();

        const bool tick_subscribers = frame_clock_.has_active_subscribers();
        const bool animate_css = tree_has_active_css_animation(&root_);
        const bool gesture_work = root_.has_time_driven_gestures();
        if (tick_subscribers || animate_css || gesture_work) {
            if (tick_subscribers)
                frame_clock_.tick(1.0f / 60.0f);
            if (animate_css)
                root_.tick_animations(1.0f / 60.0f);
            if (gesture_work)
                root_.advance_gesture_recognizers();
            needs_repaint_.store(true, std::memory_order_relaxed);
        }
        if (needs_repaint_.exchange(false, std::memory_order_relaxed)) {
            render_frame();
        }
    }

    void set_size(uint32_t w, uint32_t h) override {
        size_ = {w, h};
        @autoreleasepool {
            metal_view_.frame = CGRectMake(0, 0, w, h);
            scale_ = current_scale();
            [metal_view_ metalLayer].contentsScale = scale_;
            [metal_view_ metalLayer].drawableSize = CGSizeMake(w * scale_, h * scale_);
            // Resize parity: GpuSurface PHYSICAL, SkiaSurface LOGICAL + scale.
            if (gpu_surface_) {
                gpu_surface_->resize(static_cast<uint32_t>(w * scale_),
                                     static_cast<uint32_t>(h * scale_));
            }
            if (skia_surface_) {
                skia_surface_->resize(w, h, static_cast<float>(scale_));
            }
        }
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    Size get_size() const override { return size_; }

    bool is_gpu_backed() const override {
        return gpu_surface_ != nullptr && skia_surface_ != nullptr &&
               skia_surface_->is_available();
    }

    // Mirrors WindowHost::gpu_surface() so a scripted UI mounted inside an
    // AUv3 editor can route navigator.gpu / canvas.getContext('webgpu')
    // through the same wgpu::Surface that paints the editor.
    render::GpuSurface* gpu_surface() const override {
        return gpu_surface_.get();
    }

    void set_idle_callback(std::function<void()> callback) override {
        idle_callback_ = std::move(callback);
    }

    // ── Design viewport (mirrors MacGpuPluginViewHost) ──────────────────────
    // The AU iOS view controller pins the editor to the plug-in's design size
    // (e.g. the Three.js demo's 540×720 shell) and lets the host scale that up
    // to the editor pane. Before this override these calls were base no-ops, so
    // the design rendered at native size in the pane's top-left. render_frame()
    // now lays the root out at the design size and applies an aspect-correct
    // translate+scale; touch points get the inverse via window_to_root_point().
    void set_fixed_aspect_ratio(float ratio) override {
        // Plugin hosts don't own the OS window — the DAW enforces aspect via
        // the per-format resize-hint path. Stored for API parity.
        fixed_aspect_ratio_ = ratio;
    }

    void set_design_viewport(float design_w, float design_h) override {
        design_viewport_w_ = design_w;
        design_viewport_h_ = design_h;
        @autoreleasepool {
            if (design_w > 0.0f && design_h > 0.0f) {
                __block IOSGpuPluginViewHost* host = this;
                metal_view_.pointTransform = ^pulp::view::Point(pulp::view::Point pt) {
                    return host->window_to_root_point(pt);
                };
            } else {
                metal_view_.pointTransform = nil;
            }
        }
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    void set_design_viewport_top_align(bool top_align) override {
        design_top_align_ = top_align;
        needs_repaint_.store(true, std::memory_order_relaxed);
    }

    Point window_to_root_point(Point pt) const override {
        float sx, sy, tx, ty;
        if (!WindowHost::compute_design_viewport_transform(
                static_cast<float>(size_.width),
                static_cast<float>(size_.height),
                design_viewport_w_, design_viewport_h_,
                sx, sy, tx, ty, design_top_align_)) {
            return pt;
        }
        if (sx <= 0.0f || sy <= 0.0f) return pt;
        return { (pt.x - tx) / sx, (pt.y - ty) / sy };
    }

    bool attach_native_child_view(NativeViewHandle child_view,
                                  float x,
                                  float y,
                                  float width,
                                  float height) override {
        return attach_child_view_to_host(metal_view_, child_view, x, y, width, height);
    }

    bool set_native_child_view_bounds(NativeViewHandle child_view,
                                      float x,
                                      float y,
                                      float width,
                                      float height) override {
        return set_child_view_bounds_in_host(metal_view_, child_view, x, y, width, height);
    }

    void detach_native_child_view(NativeViewHandle child_view) override {
        detach_child_view_from_host(metal_view_, child_view);
    }

    bool set_native_child_view_clip(NativeViewHandle child_view,
                                    bool has_clip,
                                    float x,
                                    float y,
                                    float width,
                                    float height) override {
        return clip_child_view_in_host(metal_view_, child_view, has_clip, x, y,
                                       width, height);
    }

private:
    View& root_;
    Size size_;
    PulpMetalPluginView* metal_view_ = nil;
    PulpIOSDragDrop* drag_drop_ = nil;
    std::unique_ptr<render::GpuSurface> gpu_surface_;
    std::unique_ptr<render::SkiaSurface> skia_surface_;
    FrameClock frame_clock_;
    std::atomic<bool> needs_repaint_{false};
    CADisplayLink* display_link_ = nil;
    PulpIOSPluginDisplayLinkTarget* display_link_target_ = nil;
    std::function<void()> idle_callback_;
    CGFloat scale_ = 1.0;
    // Design viewport: when (>0, >0) the root paints at design size and the
    // Skia canvas applies translate+scale to fit the host bounds; touch coords
    // are inverse-mapped via window_to_root_point().
    float design_viewport_w_ = 0.0f;
    float design_viewport_h_ = 0.0f;
    float fixed_aspect_ratio_ = 0.0f;
    bool design_top_align_ = false;
    // Liveness token (parity with the mac host). CADisplayLink ticks run on
    // the main run loop and stop on -invalidate, but the token makes a racing
    // teardown safe and matches the cross-platform contract.
    std::shared_ptr<std::atomic<bool>> alive_;

    // Prefer the view's window-screen scale; fall back to the view's own
    // content scale, then the main screen. Updated on layout / window change.
    CGFloat current_scale() const {
        if (metal_view_.window && metal_view_.window.screen)
            return metal_view_.window.screen.scale;
        if (metal_view_)
            return metal_view_.contentScaleFactor;
        return UIScreen.mainScreen.scale;
    }

    static bool tree_has_active_css_animation(View* view) {
        if (!view) return false;
        if (view->animation_play_state() != "paused") {
            for (const auto& a : view->active_animations()) {
                if (a.active) return true;
            }
        }
        for (size_t i = 0; i < view->child_count(); ++i) {
            if (tree_has_active_css_animation(view->child_at(i))) return true;
        }
        return false;
    }

    void handle_window_change() {
        if (metal_view_.window) {
            scale_ = current_scale();
            start_display_link();
            needs_repaint_.store(true, std::memory_order_relaxed);
        } else {
            stop_display_link();
        }
    }

    void handle_layout() {
        const CGFloat new_scale = current_scale();
        const CGSize bounds = metal_view_.bounds.size;
        const uint32_t w = static_cast<uint32_t>(bounds.width);
        const uint32_t h = static_cast<uint32_t>(bounds.height);
        if (new_scale != scale_ || w != size_.width || h != size_.height) {
            if (w > 0 && h > 0) set_size(w, h);
        }
    }

    void start_display_link() {
        if (display_link_) return;
        @autoreleasepool {
            display_link_target_ = [[PulpIOSPluginDisplayLinkTarget alloc] init];
            display_link_target_.host = this;
            display_link_ = [CADisplayLink displayLinkWithTarget:display_link_target_
                                                        selector:@selector(tick:)];
            [display_link_ addToRunLoop:[NSRunLoop mainRunLoop]
                                forMode:NSRunLoopCommonModes];
        }
    }

    void stop_display_link() {
        @autoreleasepool {
            if (display_link_) {
                [display_link_ invalidate];
                display_link_ = nil;
            }
            if (display_link_target_) {
                display_link_target_.host = nullptr;
                display_link_target_ = nil;
            }
        }
    }

    void render_frame() {
        if (!gpu_surface_ || !skia_surface_) return;
        if (!gpu_surface_->begin_frame()) return;

        auto* cv = skia_surface_->begin_frame();
        if (cv) {
            const float bw = static_cast<float>(size_.width);
            const float bh = static_cast<float>(size_.height);
            // Clear at host bounds first so any design-viewport letterbox bars
            // share the design background color.
            cv->set_fill_color(canvas::Color::rgba8(30, 30, 46));
            cv->fill_rect(0, 0, bw, bh);

            float sx, sy, tx, ty;
            const bool has_viewport =
                design_viewport_w_ > 0.0f && design_viewport_h_ > 0.0f &&
                WindowHost::compute_design_viewport_transform(
                    bw, bh, design_viewport_w_, design_viewport_h_,
                    sx, sy, tx, ty, design_top_align_);

            if (has_viewport) {
                // Pin the root to the design size and scale the painted output
                // up to the host bounds. Mouse/touch input inverse-maps
                // window→root via window_to_root_point() before hit_test, so
                // paint + input stay consistent. paint_overlays MUST run inside
                // the transform (overlays draw in root coords). Mirrors
                // MacGpuPluginViewHost::paint_scene.
                root_.set_bounds({0, 0, design_viewport_w_, design_viewport_h_});
                root_.layout_children();
                const int saved = cv->save_count();
                cv->save();
                cv->translate(tx, ty);
                cv->scale(sx, sy);
                root_.paint_all(*cv);
                View::paint_overlays(*cv, &root_);
                cv->restore_to_count(saved);
            } else {
                root_.set_bounds({0, 0, bw, bh});
                root_.layout_children();
                root_.paint_all(*cv);
                View::paint_overlays(*cv, &root_);
            }
        }

        skia_surface_->end_frame();
        gpu_surface_->end_frame();
    }
};

}  // namespace pulp::view

@implementation PulpIOSPluginDisplayLinkTarget
- (void)tick:(CADisplayLink*)link {
    (void)link;
    if (_host) _host->tick();
}
@end

#endif // PULP_HAS_SKIA

namespace pulp::view {

// Factory functions
std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, Size size) {
    auto host = std::make_unique<IOSPluginViewHost>(root, size);
    root.set_plugin_view_host(host.get());
    return host;
}

std::unique_ptr<PluginViewHost> PluginViewHost::create(View& root, const Options& options) {
#ifdef PULP_HAS_SKIA
    if (options.use_gpu) {
        auto host = std::make_unique<IOSGpuPluginViewHost>(root, options);
        if (host->is_gpu_backed()) {
            root.set_plugin_view_host(host.get());
            return host;
        }
        // GPU init failed — fall back to the CoreGraphics host so the editor
        // never disappears. The runtime scream-guard in the adapter
        // (`warn_if_unexpected_cpu_fallback`) logs the mismatch loudly.
        host.reset();
    }
#endif
    auto host = std::make_unique<IOSPluginViewHost>(root, options.size);
    root.set_plugin_view_host(host.get());
    return host;
}

} // namespace pulp::view

#endif // TARGET_OS_IOS
