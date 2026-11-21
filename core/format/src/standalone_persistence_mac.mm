#include <pulp/format/detail/standalone_persistence.hpp>

#import <Foundation/Foundation.h>

namespace pulp::format::detail {

void configure_standalone_persistence(bool headless) {
    if (!headless)
        return;

    // AppKit reads these defaults while it restores windows during launch.
    // Setting them in the argument domain before NSApplication exists avoids the modal crash
    // recovery prompt that cannot be answered by a headless process.
    [[NSUserDefaults standardUserDefaults] setVolatileDomain:@{
        @"ApplePersistenceIgnoreState" : @YES,
        @"NSQuitAlwaysKeepsWindows" : @NO,
    }
                                                     forName:NSArgumentDomain];
}

} // namespace pulp::format::detail
