// Ship-safety symbol-absence fixture.
//
// Built twice by test/cmake/format_reload_tests.cmake: a DEV binary (watcher
// gated ON + the fixture force-emit mark) and a SHIP binary
// (PULP_RELOAD_DEV_WATCHER=0). The reload_ship_symbol_check.cmake test nm's
// both and asserts the dev filesystem watcher symbol, the environment path
// resolver, and the PULP_RELOAD_LOGIC_PATH literal are present in dev and
// ABSENT in ship — proving a shipping build carries no filesystem-watch /
// auto-dlopen entry point and no environment-chosen load path.
#include <pulp/format/reload/reloadable_shell.hpp>

int main() {
    pulp::format::reload::ReloadableShell shell;  // force class emission
    (void)shell.descriptor();
    return 0;
}
