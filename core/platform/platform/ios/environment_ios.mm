// iOS adapter for the unified Environment API (#342).
//
// Subscribes to UIApplication / UIScreen / UIDevice notifications and
// snapshots the OS state into Environment::publish(). Listeners outside
// the platform layer don't see any UIKit types — Environment is pure C++.
//
// Hooks:
//   UIApplicationDidBecomeActiveNotification     → lifecycle (foreground)
//   UIApplicationWillResignActiveNotification    → lifecycle (inactive)
//   UIApplicationDidEnterBackgroundNotification  → lifecycle (background)
//   UIApplicationWillEnterForegroundNotification → lifecycle (inactive)
//   UIApplicationDidReceiveMemoryWarningNotification → memory pressure (critical)
//   UIDeviceOrientationDidChangeNotification     → orientation
//   UIKeyboardWillShowNotification               → keyboard inset
//   UIKeyboardWillHideNotification               → keyboard inset (zeroed)
//   KVO on UITraitCollection (via traitCollectionDidChange) is owned by
//   the host's root view controller; we observe color scheme by reading
//   UIScreen.mainScreen.traitCollection on every publish so any layout
//   relayout that nudges us picks up the latest value too.
//
// Display geometry is read from UIScreen.mainScreen on every publish.
// Safe-area insets come from the application's key window. Keyboard
// inset comes from the UIKeyboard* notifications' userInfo dictionaries.
//
// Codebase compiles .mm files in MRR — no ARC, no __weak. The observer
// is a process-lived singleton created via dispatch_once and never
// released, so capturing self by reference inside blocks is safe.

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <pulp/platform/environment.hpp>

using pulp::platform::ColorScheme;
using pulp::platform::DisplayInfo;
using pulp::platform::Environment;
using pulp::platform::EnvironmentState;
using pulp::platform::KeyboardInset;
using pulp::platform::LifecycleState;
using pulp::platform::MemoryPressure;
using pulp::platform::Orientation;
using pulp::platform::SafeAreaInsets;

namespace {

DisplayInfo snapshot_main_display() {
    DisplayInfo info;
    // [UIScreen mainScreen] is deprecated in iOS 26 in favor of
    // view.window.windowScene.screen, but the Environment observer is
    // a process-level singleton with no view context — falling back to
    // the main screen matches every other multi-window iOS API and is
    // accurate for single-screen apps (the only configuration iOS
    // currently supports outside Stage Manager).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    UIScreen* screen = [UIScreen mainScreen];
#pragma clang diagnostic pop
    if (!screen) return info;
    CGRect bounds = screen.bounds;
    info.width  = static_cast<float>(bounds.size.width);
    info.height = static_cast<float>(bounds.size.height);
    info.scale  = static_cast<float>(screen.nativeScale);
    info.physical_width  = static_cast<int>(bounds.size.width  * info.scale);
    info.physical_height = static_cast<int>(bounds.size.height * info.scale);
    // maximumFramesPerSecond is the panel cap; ProMotion devices report
    // 120, standard panels 60. Zero means unknown.
    info.refresh_hz = static_cast<float>(screen.maximumFramesPerSecond);
    // UIScreen has no localizedName equivalent; leave info.name empty.
    return info;
}

ColorScheme detect_color_scheme() {
    UITraitCollection* traits = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    UIScreen* screen = [UIScreen mainScreen];
#pragma clang diagnostic pop
    if (screen) traits = screen.traitCollection;
    if (!traits) return ColorScheme::unknown;
    switch (traits.userInterfaceStyle) {
        case UIUserInterfaceStyleDark:  return ColorScheme::dark;
        case UIUserInterfaceStyleLight: return ColorScheme::light;
        default:                        return ColorScheme::unknown;
    }
}

UIWindow* key_window() {
    // UIApplication.keyWindow is deprecated; iterate scenes' windows.
    UIApplication* app = [UIApplication sharedApplication];
    if (!app) return nil;
    if (@available(iOS 13.0, *)) {
        for (UIScene* scene in app.connectedScenes) {
            if (scene.activationState != UISceneActivationStateForegroundActive)
                continue;
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            UIWindowScene* ws = (UIWindowScene*)scene;
            for (UIWindow* w in ws.windows) {
                if (w.isKeyWindow) return w;
            }
            // Fall back to the first window of the active scene.
            if (ws.windows.count > 0) return ws.windows.firstObject;
        }
    }
    // Last resort for pre-iOS 13 builds: iterate top-level windows.
    // UIApplication.windows is deprecated in iOS 15 but the @available
    // block above already covers iOS 13+; this branch is unreachable on
    // any supported iOS deployment target.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    for (UIWindow* w in app.windows) {
        if (w.isKeyWindow) return w;
    }
    return app.windows.firstObject;
#pragma clang diagnostic pop
}

SafeAreaInsets snapshot_safe_area() {
    SafeAreaInsets insets;
    UIWindow* w = key_window();
    if (!w) return insets;
    UIEdgeInsets ei = w.safeAreaInsets;
    insets.top    = static_cast<float>(ei.top);
    insets.bottom = static_cast<float>(ei.bottom);
    insets.left   = static_cast<float>(ei.left);
    insets.right  = static_cast<float>(ei.right);
    return insets;
}

Orientation map_orientation(UIDeviceOrientation o) {
    switch (o) {
        case UIDeviceOrientationPortrait:           return Orientation::portrait;
        case UIDeviceOrientationPortraitUpsideDown: return Orientation::portrait_upside_down;
        case UIDeviceOrientationLandscapeLeft:      return Orientation::landscape_left;
        case UIDeviceOrientationLandscapeRight:     return Orientation::landscape_right;
        case UIDeviceOrientationFaceUp:
        case UIDeviceOrientationFaceDown:           return Orientation::flat;
        case UIDeviceOrientationUnknown:
        default:                                    return Orientation::unknown;
    }
}

Orientation snapshot_orientation() {
    UIDevice* device = [UIDevice currentDevice];
    if (!device) return Orientation::unknown;
    return map_orientation(device.orientation);
}

} // namespace

@interface PulpEnvironmentObserverIos : NSObject
@property(nonatomic) pulp::platform::LifecycleState  currentLifecycle;
@property(nonatomic) pulp::platform::MemoryPressure  currentPressure;
@property(nonatomic) pulp::platform::KeyboardInset   currentKeyboard;
- (void)startObserving;
- (void)publishSnapshot;
@end

@implementation PulpEnvironmentObserverIos

- (instancetype)init {
    if ((self = [super init])) {
        _currentLifecycle = LifecycleState::foreground;
        _currentPressure  = MemoryPressure::normal;
        _currentKeyboard  = KeyboardInset{};
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    UIDevice* device = [UIDevice currentDevice];
    if (device && device.isGeneratingDeviceOrientationNotifications) {
        [device endGeneratingDeviceOrientationNotifications];
    }
    [super dealloc];
}

- (void)startObserving {
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    // Lifecycle.
    [nc addObserver:self
           selector:@selector(onDidBecomeActive:)
               name:UIApplicationDidBecomeActiveNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onWillResignActive:)
               name:UIApplicationWillResignActiveNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onDidEnterBackground:)
               name:UIApplicationDidEnterBackgroundNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onWillEnterForeground:)
               name:UIApplicationWillEnterForegroundNotification
             object:nil];

    // Memory pressure (iOS surfaces only "critical" via this notification).
    [nc addObserver:self
           selector:@selector(onMemoryWarning:)
               name:UIApplicationDidReceiveMemoryWarningNotification
             object:nil];

    // Orientation. iOS only emits these once we explicitly start
    // generating them; the host might already have asked, in which case
    // a second beginGenerating... call still nests safely (refcounted).
    UIDevice* device = [UIDevice currentDevice];
    if (device) {
        [device beginGeneratingDeviceOrientationNotifications];
        [nc addObserver:self
               selector:@selector(onOrientationChange:)
                   name:UIDeviceOrientationDidChangeNotification
                 object:nil];
    }

    // Keyboard.
    [nc addObserver:self
           selector:@selector(onKeyboardWillShow:)
               name:UIKeyboardWillShowNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onKeyboardWillHide:)
               name:UIKeyboardWillHideNotification
             object:nil];

    // Display: UIScreenModeDidChange + brightness aren't strict matches
    // for "geometry changed", but bounds shifts always coincide with
    // either an orientation change or a window scene resize. We snapshot
    // display on every publish, so orientation/keyboard events also pick
    // up new geometry without an extra observer.

    [self publishSnapshot];
}

- (void)publishSnapshot {
    EnvironmentState s;
    s.display         = snapshot_main_display();
    s.safe_area       = snapshot_safe_area();
    s.keyboard        = _currentKeyboard;
    s.orientation     = snapshot_orientation();
    s.color_scheme    = detect_color_scheme();
    s.lifecycle       = _currentLifecycle;
    s.memory_pressure = _currentPressure;
    Environment::instance().publish(s);
}

// ── Lifecycle ───────────────────────────────────────────────────────
- (void)onDidBecomeActive:(NSNotification*)note {
    _currentLifecycle = LifecycleState::foreground;
    [self publishSnapshot];
}
- (void)onWillResignActive:(NSNotification*)note {
    _currentLifecycle = LifecycleState::inactive;
    [self publishSnapshot];
}
- (void)onDidEnterBackground:(NSNotification*)note {
    _currentLifecycle = LifecycleState::background;
    [self publishSnapshot];
}
- (void)onWillEnterForeground:(NSNotification*)note {
    // The corresponding DidBecomeActive will follow shortly and clamp
    // us back to foreground; report inactive in the interim so layout
    // can react before the activation completes.
    _currentLifecycle = LifecycleState::inactive;
    [self publishSnapshot];
}

// ── Memory pressure ─────────────────────────────────────────────────
- (void)onMemoryWarning:(NSNotification*)note {
    // iOS only emits a single "critical" tier via this notification.
    // We keep the field at critical until the next publish overrides
    // it (e.g. on the next lifecycle/orientation event), matching
    // Android's didReceiveMemoryWarning semantics.
    _currentPressure = MemoryPressure::critical;
    [self publishSnapshot];
    // Reset so future snapshots don't keep reporting critical forever.
    _currentPressure = MemoryPressure::normal;
}

// ── Orientation ─────────────────────────────────────────────────────
- (void)onOrientationChange:(NSNotification*)note {
    [self publishSnapshot];
}

// ── Keyboard ────────────────────────────────────────────────────────
- (void)onKeyboardWillShow:(NSNotification*)note {
    NSDictionary* info = note.userInfo;
    if (!info) {
        [self publishSnapshot];
        return;
    }
    NSValue* frameValue = info[UIKeyboardFrameEndUserInfoKey];
    NSNumber* duration  = info[UIKeyboardAnimationDurationUserInfoKey];
    KeyboardInset k;
    if (frameValue) {
        CGRect endFrame = frameValue.CGRectValue;
        // The keyboard frame is in screen coordinates; its height is the
        // bottom inset for layout purposes.
        k.bottom = static_cast<float>(endFrame.size.height);
    }
    if (duration) {
        k.animation_duration = static_cast<float>(duration.doubleValue);
    }
    _currentKeyboard = k;
    [self publishSnapshot];
}

- (void)onKeyboardWillHide:(NSNotification*)note {
    NSDictionary* info = note.userInfo;
    KeyboardInset k;
    if (info) {
        NSNumber* duration = info[UIKeyboardAnimationDurationUserInfoKey];
        if (duration) {
            k.animation_duration = static_cast<float>(duration.doubleValue);
        }
    }
    _currentKeyboard = k; // bottom defaults to 0
    [self publishSnapshot];
}
@end

namespace pulp::platform {

namespace {
PulpEnvironmentObserverIos* g_observer = nil;
}

// Public entry — host bootstrap calls this once during UIApplication
// setup. Idempotent: a second call is a no-op so AUv3/standalone host
// loads don't double-register observers.
void start_environment_observer_ios() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // The observer is process-lived; we deliberately never release
        // it. dispatch_once guarantees a single instance per process.
        void (^bootstrap)(void) = ^{
            g_observer = [[PulpEnvironmentObserverIos alloc] init];
            [g_observer startObserving];
        };
        if ([NSThread isMainThread]) {
            bootstrap();
        } else {
            // UIKit observers must be installed on the main thread.
            dispatch_sync(dispatch_get_main_queue(), bootstrap);
        }
    });
}

} // namespace pulp::platform
