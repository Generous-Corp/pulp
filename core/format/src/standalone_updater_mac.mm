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

namespace {

// Sparkle's SPUUpdater, once the controller exists; nil before that.
id sparkle_updater() {
    if (g_updater_controller == nil)
        return nil;
    SEL updater_sel = NSSelectorFromString(@"updater");
    if (![g_updater_controller respondsToSelector:updater_sel])
        return nil;
    using GetFn = id (*)(id, SEL);
    return reinterpret_cast<GetFn>(objc_msgSend)(g_updater_controller, updater_sel);
}

bool send_bool_getter(id object, NSString* name, bool fallback) {
    SEL sel = NSSelectorFromString(name);
    if (object == nil || ![object respondsToSelector:sel])
        return fallback;
    using GetFn = BOOL (*)(id, SEL);
    return reinterpret_cast<GetFn>(objc_msgSend)(object, sel) == YES;
}

std::string info_string(NSString* key) {
    id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:key];
    if (![value isKindOfClass:[NSString class]])
        return {};
    const char* utf8 = [(NSString*)value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

// An Info.plist boolean, or `fallback` when the key is absent. Sparkle's own
// keys are booleans; tolerate the "YES"/"NO" strings a hand-edited plist has.
bool info_bool(NSString* key, bool fallback) {
    id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:key];
    if ([value respondsToSelector:@selector(boolValue)])
        return [value boolValue] == YES;
    return fallback;
}

// Sparkle keeps the user's choices in the app's standard user defaults, under
// the same key names as Info.plist; a user default wins over the plist.
bool sparkle_setting(NSString* key, bool fallback) {
    id value = [[NSUserDefaults standardUserDefaults] objectForKey:key];
    if ([value respondsToSelector:@selector(boolValue)])
        return [value boolValue] == YES;
    return info_bool(key, fallback);
}

class SparkleUpdateService final : public AppUpdateService {
  public:
    AppUpdateStatus status() const override {
        AppUpdateStatus status;
        @autoreleasepool {
            status.available = true;
            status.app_name = info_string(@"CFBundleDisplayName");
            if (status.app_name.empty())
                status.app_name = info_string(@"CFBundleName");
            status.version = info_string(@"CFBundleShortVersionString");
            status.build = info_string(@"CFBundleVersion");
            status.releases_url = info_string(@"PulpUpdatesReleasesURL");
            const std::string feed = info_string(@"SUFeedURL");
            if (NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:feed.c_str()]];
                url != nil && url.host != nil)
                status.feed_host = [url.host UTF8String];
            const std::string installer = info_string(@"PulpUpdatesInstaller");
            status.installer = installer == "package" ? AppUpdateInstaller::package
                               : installer == "app"   ? AppUpdateInstaller::app_bundle
                                                      : AppUpdateInstaller::unknown;

            id updater = sparkle_updater();
            // Before the controller exists (a development build that has not
            // been asked to check yet) the persisted settings are the truth.
            status.automatic_checks =
                updater != nil ? send_bool_getter(updater, @"automaticallyChecksForUpdates", false)
                               : sparkle_setting(@"SUEnableAutomaticChecks", false);
            const bool allows_automatic_install = info_bool(@"SUAllowsAutomaticUpdates", true);
            const bool downloads_automatically =
                updater != nil ? send_bool_getter(updater, @"automaticallyDownloadsUpdates", false)
                               : sparkle_setting(@"SUAutomaticallyUpdate", false);
            status.automatic_install = allows_automatic_install && downloads_automatically &&
                                       status.installer != AppUpdateInstaller::package;
            // A package that needs an administrator password is never
            // installed silently, and with SUAllowsAutomaticUpdates off Sparkle
            // never offers automatic installation at all. Report the declared
            // upper bound so the note never under-promises a prompt.
            if (!allows_automatic_install)
                status.automatic_install = false;
            status.can_check_now =
                updater != nil ? send_bool_getter(updater, @"canCheckForUpdates", true) : true;

            NSDate* last = nil;
            SEL last_sel = NSSelectorFromString(@"lastUpdateCheckDate");
            if (updater != nil && [updater respondsToSelector:last_sel]) {
                using GetFn = id (*)(id, SEL);
                last = reinterpret_cast<GetFn>(objc_msgSend)(updater, last_sel);
            } else {
                id stored = [[NSUserDefaults standardUserDefaults] objectForKey:@"SULastCheckTime"];
                if ([stored isKindOfClass:[NSDate class]])
                    last = stored;
            }
            if ([last isKindOfClass:[NSDate class]])
                status.last_check_unix_seconds =
                    static_cast<std::int64_t>([last timeIntervalSince1970]);
        }
        return status;
    }

    bool check_for_updates() override {
        check_for_standalone_updates();
        return g_updater_controller != nil;
    }

    bool set_automatic_checks(bool on) override {
        @autoreleasepool {
            if (id updater = sparkle_updater()) {
                SEL set_sel = NSSelectorFromString(@"setAutomaticallyChecksForUpdates:");
                if (![updater respondsToSelector:set_sel])
                    return false;
                using SetFn = void (*)(id, SEL, BOOL);
                reinterpret_cast<SetFn>(objc_msgSend)(updater, set_sel, on ? YES : NO);
                return true;
            }
            // Not started yet: write the preference where Sparkle reads it at
            // start, exactly as SPUUpdater's own setter persists it.
            [[NSUserDefaults standardUserDefaults] setBool:on ? YES : NO
                                                    forKey:@"SUEnableAutomaticChecks"];
            return true;
        }
    }

    bool open_releases_page() override {
        @autoreleasepool {
            const std::string page = info_string(@"PulpUpdatesReleasesURL");
            if (page.rfind("https://", 0) != 0)
                return false;
            NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:page.c_str()]];
            return url != nil && [[NSWorkspace sharedWorkspace] openURL:url];
        }
    }
};

} // namespace

std::shared_ptr<AppUpdateService>
make_standalone_update_service(const StandaloneUpdaterPlan& plan) {
    switch (plan.backend) {
    case StandaloneUpdaterBackend::sparkle:
        return std::make_shared<SparkleUpdateService>();
    case StandaloneUpdaterBackend::stub: {
        @autoreleasepool {
            std::string name = info_string(@"CFBundleName");
            if (name.empty())
                name = [[[NSProcessInfo processInfo] processName] UTF8String];
            return make_stub_update_service(std::move(name),
                                            info_string(@"CFBundleShortVersionString"));
        }
    }
    case StandaloneUpdaterBackend::none:
        break;
    }
    return nullptr;
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
std::shared_ptr<AppUpdateService>
make_standalone_update_service(const StandaloneUpdaterPlan& plan) {
    if (plan.backend == StandaloneUpdaterBackend::stub)
        return make_stub_update_service("Pulp", {});
    return nullptr;
}
} // namespace pulp::format::detail

#endif
