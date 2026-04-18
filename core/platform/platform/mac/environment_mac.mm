// macOS adapter for the unified Environment API (#342).
//
// Subscribes to NSApp + NSScreen notifications, snapshots the OS state
// into Environment::publish(). Listeners outside the platform layer
// don't see any AppKit types — Environment is pure C++.
//
// Hooks:
//   NSApplicationDidChangeScreenParametersNotification → display
//   NSApplicationDidBecomeActiveNotification          → lifecycle (foreground)
//   NSApplicationDidResignActiveNotification          → lifecycle (inactive)
//   NSApplicationDidHideNotification                  → lifecycle (background)
//   NSApplicationDidUnhideNotification                → lifecycle (foreground)
//   KVO on NSApp.effectiveAppearance                  → color scheme
//
// Memory pressure on macOS is sourced from the dispatch source
// DISPATCH_SOURCE_TYPE_MEMORYPRESSURE so we don't depend on AppKit
// being initialized before that signal arrives.
//
// Safe-area / keyboard / orientation are mobile-only — desktop
// macOS leaves them at default (zero / unknown / zero), per the
// EnvironmentState contract.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <pulp/platform/environment.hpp>
#include <dispatch/dispatch.h>

using pulp::platform::ColorScheme;
using pulp::platform::DisplayInfo;
using pulp::platform::Environment;
using pulp::platform::EnvironmentState;
using pulp::platform::LifecycleState;
using pulp::platform::MemoryPressure;

namespace {

DisplayInfo snapshot_main_display() {
    DisplayInfo info;
    NSScreen* screen = [NSScreen mainScreen];
    if (!screen) return info;
    NSRect frame = screen.frame;
    info.width  = static_cast<float>(frame.size.width);
    info.height = static_cast<float>(frame.size.height);
    info.scale  = static_cast<float>(screen.backingScaleFactor);
    info.physical_width  = static_cast<int>(frame.size.width  * info.scale);
    info.physical_height = static_cast<int>(frame.size.height * info.scale);
    if (@available(macOS 12.0, *)) {
        // maximumFramesPerSecond is the panel cap; for ProMotion this
        // returns 120, for standard 60. Zero means unknown.
        info.refresh_hz = static_cast<float>(screen.maximumFramesPerSecond);
    }
    if (NSString* name = screen.localizedName) {
        info.name = name.UTF8String ? name.UTF8String : "";
    }
    return info;
}

ColorScheme detect_color_scheme() {
    NSAppearance* appearance = nil;
    if (NSApp) appearance = NSApp.effectiveAppearance;
    if (!appearance) appearance = [NSAppearance currentDrawingAppearance];
    if (!appearance) return ColorScheme::unknown;
    NSAppearanceName best = [appearance bestMatchFromAppearancesWithNames:@[
        NSAppearanceNameAqua,
        NSAppearanceNameDarkAqua
    ]];
    if ([best isEqualToString:NSAppearanceNameDarkAqua]) return ColorScheme::dark;
    if ([best isEqualToString:NSAppearanceNameAqua])     return ColorScheme::light;
    return ColorScheme::unknown;
}

void publish_full_snapshot(LifecycleState lifecycle, MemoryPressure pressure) {
    EnvironmentState s;
    s.display      = snapshot_main_display();
    s.color_scheme = detect_color_scheme();
    s.lifecycle    = lifecycle;
    s.memory_pressure = pressure;
    Environment::instance().publish(s);
}

} // namespace

@interface PulpEnvironmentObserver : NSObject
@property(nonatomic) pulp::platform::LifecycleState currentLifecycle;
@property(nonatomic) pulp::platform::MemoryPressure currentPressure;
- (void)startObserving;
@end

@implementation PulpEnvironmentObserver {
    dispatch_source_t _pressureSource;
}

- (instancetype)init {
    if ((self = [super init])) {
        _currentLifecycle = LifecycleState::foreground;
        _currentPressure  = MemoryPressure::normal;
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    if (NSApp) {
        @try { [NSApp removeObserver:self forKeyPath:@"effectiveAppearance"]; }
        @catch (NSException*) {}
    }
    if (_pressureSource) {
        dispatch_source_cancel(_pressureSource);
        _pressureSource = nullptr;
    }
}

- (void)startObserving {
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    [nc addObserver:self
           selector:@selector(onScreenChange:)
               name:NSApplicationDidChangeScreenParametersNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onActive:)
               name:NSApplicationDidBecomeActiveNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onResign:)
               name:NSApplicationDidResignActiveNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onHide:)
               name:NSApplicationDidHideNotification
             object:nil];
    [nc addObserver:self
           selector:@selector(onUnhide:)
               name:NSApplicationDidUnhideNotification
             object:nil];

    if (NSApp) {
        [NSApp addObserver:self
                forKeyPath:@"effectiveAppearance"
                   options:0
                   context:nullptr];
    }

    // Memory pressure dispatch source. Coalesces warning + critical so
    // a transition warning -> critical becomes two publish() calls.
    _pressureSource = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
        dispatch_get_main_queue());
    if (_pressureSource) {
        // Observer is process-lived (created via dispatch_once and never
        // released), so capturing self by reference is safe. The block
        // also keeps it alive while the dispatch source is active.
        // Codebase compiles .mm files in MRR — no __weak available.
        dispatch_source_t source = _pressureSource;
        PulpEnvironmentObserver* observerRef = self;
        dispatch_source_set_event_handler(_pressureSource, ^{
            unsigned long flags = dispatch_source_get_data(source);
            if (flags & DISPATCH_MEMORYPRESSURE_CRITICAL) {
                observerRef.currentPressure = MemoryPressure::critical;
            } else if (flags & DISPATCH_MEMORYPRESSURE_WARN) {
                observerRef.currentPressure = MemoryPressure::moderate;
            } else {
                observerRef.currentPressure = MemoryPressure::normal;
            }
            publish_full_snapshot(observerRef.currentLifecycle,
                                  observerRef.currentPressure);
        });
        dispatch_resume(_pressureSource);
    }

    publish_full_snapshot(_currentLifecycle, _currentPressure);
}

- (void)onScreenChange:(NSNotification*)note {
    publish_full_snapshot(_currentLifecycle, _currentPressure);
}
- (void)onActive:(NSNotification*)note {
    _currentLifecycle = LifecycleState::foreground;
    publish_full_snapshot(_currentLifecycle, _currentPressure);
}
- (void)onResign:(NSNotification*)note {
    _currentLifecycle = LifecycleState::inactive;
    publish_full_snapshot(_currentLifecycle, _currentPressure);
}
- (void)onHide:(NSNotification*)note {
    _currentLifecycle = LifecycleState::background;
    publish_full_snapshot(_currentLifecycle, _currentPressure);
}
- (void)onUnhide:(NSNotification*)note {
    _currentLifecycle = LifecycleState::foreground;
    publish_full_snapshot(_currentLifecycle, _currentPressure);
}

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey,id>*)change
                       context:(void*)context {
    if ([keyPath isEqualToString:@"effectiveAppearance"]) {
        publish_full_snapshot(_currentLifecycle, _currentPressure);
    }
}
@end

namespace pulp::platform {

namespace {
PulpEnvironmentObserver* g_observer = nil;
}

// Public entry — host bootstrap calls this once during NSApp setup.
// Idempotent: a second call is a no-op so AU/VST3/CLAP host loads
// don't double-register observers.
void start_environment_observer_mac() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_observer = [[PulpEnvironmentObserver alloc] init];
        [g_observer startObserving];
    });
}

} // namespace pulp::platform
