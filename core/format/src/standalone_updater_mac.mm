#include <pulp/format/detail/standalone_updater.hpp>

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <Cocoa/Cocoa.h>
#import <Security/Security.h>
#include <objc/message.h>

#include <cstdlib>

// Sparkle is reached only through the Objective-C runtime, so the SDK neither
// links nor includes it. The two selectors used are Sparkle 2's documented
// public API on SPUStandardUpdaterController:
//   -initWithStartingUpdater:updaterDelegate:userDriverDelegate:
//   -checkForUpdates:

namespace pulp::format::detail {
namespace {

id g_updater_controller = nil;  // retained for the life of the process

Class updater_controller_class() {
    Class cls = NSClassFromString(@"SPUStandardUpdaterController");
    if (cls == Nil) return Nil;
    // Only a Sparkle embedded in THIS app counts. The standalone host never
    // runs inside a DAW, but keep the guarantee local: a Sparkle that some
    // other bundle loaded into the process must not light up our menu.
    NSString* frameworks = [[NSBundle mainBundle] privateFrameworksPath];
    NSString* sparkle_path = [[NSBundle bundleForClass:cls] bundlePath];
    if (frameworks.length == 0 || sparkle_path.length == 0) return Nil;
    NSString* prefix = [frameworks stringByStandardizingPath];
    NSString* candidate = [sparkle_path stringByStandardizingPath];
    if (![candidate hasPrefix:[prefix stringByAppendingString:@"/"]]) return Nil;
    return cls;
}

bool main_executable_has_team_id() {
    SecCodeRef self_code = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &self_code) != errSecSuccess || !self_code)
        return false;
    SecStaticCodeRef static_code = nullptr;
    bool has_team = false;
    if (SecCodeCopyStaticCode(self_code, kSecCSDefaultFlags, &static_code) == errSecSuccess &&
        static_code) {
        CFDictionaryRef info = nullptr;
        if (SecCodeCopySigningInformation(static_code, kSecCSSigningInformation, &info) ==
                errSecSuccess &&
            info) {
            auto team = static_cast<CFStringRef>(
                CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier));
            has_team = team && CFGetTypeID(team) == CFStringGetTypeID() &&
                       CFStringGetLength(team) > 0;
            CFRelease(info);
        }
        CFRelease(static_code);
    }
    CFRelease(self_code);
    return has_team;
}

} // namespace

StandaloneUpdaterEnvironment probe_standalone_updater_environment(bool headless) {
    StandaloneUpdaterEnvironment env;
    env.headless = headless;
    @autoreleasepool {
        id feed = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUFeedURL"];
        env.bundle_declares_feed =
            [feed isKindOfClass:[NSString class]] && [(NSString*)feed length] > 0;
        env.updater_framework_loaded = updater_controller_class() != Nil;
    }
    env.developer_id_signed = main_executable_has_team_id();
    if (const char* value = std::getenv("PULP_STANDALONE_UPDATER")) env.override_value = value;
    return env;
}

void create_updater_controller();

void start_standalone_updater() {
    if (g_updater_controller != nil) return;
    // Sparkle expects to start once the application has finished launching
    // (its first scheduled check and permission prompt key off that). A
    // standalone sets the window up before entering [NSApp run], so defer to
    // the launch notification when the run loop has not started yet.
    if (NSApp == nil || ![NSApp isRunning]) {
        static bool observer_installed = false;
        if (observer_installed) return;
        observer_installed = true;
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidFinishLaunchingNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) { create_updater_controller(); }];
        return;
    }
    create_updater_controller();
}

void create_updater_controller() {
    if (g_updater_controller != nil) return;
    Class cls = updater_controller_class();
    if (cls == Nil) return;
    SEL init_sel = NSSelectorFromString(@"initWithStartingUpdater:updaterDelegate:userDriverDelegate:");
    id allocated = [cls alloc];
    if (![allocated respondsToSelector:init_sel]) return;
    using InitFn = id (*)(id, SEL, BOOL, id, id);
    g_updater_controller =
        reinterpret_cast<InitFn>(objc_msgSend)(allocated, init_sel, YES, nil, nil);
}

void check_for_standalone_updates() {
    // A menu click means the app is running; create the controller now.
    create_updater_controller();
    if (g_updater_controller == nil) return;
    SEL check_sel = NSSelectorFromString(@"checkForUpdates:");
    if (![g_updater_controller respondsToSelector:check_sel]) return;
    using CheckFn = void (*)(id, SEL, id);
    reinterpret_cast<CheckFn>(objc_msgSend)(g_updater_controller, check_sel, nil);
}

} // namespace pulp::format::detail

#else

namespace pulp::format::detail {
StandaloneUpdaterEnvironment probe_standalone_updater_environment(bool headless) {
    StandaloneUpdaterEnvironment env;
    env.headless = headless;
    return env;
}
void start_standalone_updater() {}
void check_for_standalone_updates() {}
} // namespace pulp::format::detail

#endif
