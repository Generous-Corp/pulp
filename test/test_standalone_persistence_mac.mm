#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/standalone_persistence.hpp>

#import <Foundation/Foundation.h>

namespace {

struct ScopedDefault {
    NSString* key;
    id value;

    explicit ScopedDefault(NSString* key_value)
        : key(key_value), value([[NSUserDefaults standardUserDefaults] objectForKey:key]) {}

    ~ScopedDefault() {
        if (value)
            [[NSUserDefaults standardUserDefaults] setObject:value forKey:key];
        else
            [[NSUserDefaults standardUserDefaults] removeObjectForKey:key];
    }
};

} // namespace

TEST_CASE("headless standalone opts out of AppKit window restoration",
          "[standalone][mac][headless]") {
    ScopedDefault ignore_state(@"ApplePersistenceIgnoreState");
    ScopedDefault keep_windows(@"NSQuitAlwaysKeepsWindows");
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    [defaults setBool:NO forKey:@"ApplePersistenceIgnoreState"];
    [defaults setBool:YES forKey:@"NSQuitAlwaysKeepsWindows"];

    pulp::format::detail::configure_standalone_persistence(true);

    REQUIRE([defaults boolForKey:@"ApplePersistenceIgnoreState"]);
    REQUIRE_FALSE([defaults boolForKey:@"NSQuitAlwaysKeepsWindows"]);
    [defaults setVolatileDomain:@{} forName:NSArgumentDomain];
}

TEST_CASE("interactive standalone leaves AppKit restoration defaults alone", "[standalone][mac]") {
    ScopedDefault ignore_state(@"ApplePersistenceIgnoreState");
    ScopedDefault keep_windows(@"NSQuitAlwaysKeepsWindows");
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    [defaults setVolatileDomain:@{} forName:NSArgumentDomain];
    [defaults setBool:NO forKey:@"ApplePersistenceIgnoreState"];
    [defaults setBool:YES forKey:@"NSQuitAlwaysKeepsWindows"];

    pulp::format::detail::configure_standalone_persistence(false);

    REQUIRE_FALSE([defaults boolForKey:@"ApplePersistenceIgnoreState"]);
    REQUIRE([defaults boolForKey:@"NSQuitAlwaysKeepsWindows"]);
}
