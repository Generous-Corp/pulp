// Ship-safety symbol-absence fixture (live-swap item 1.12 / D5).
//
// Built twice by test/CMakeLists.txt: a DEV binary (watcher gated ON + the
// fixture force-emit mark) and a SHIP binary (PULP_RELOAD_DEV_WATCHER=0). The
// reload_ship_symbol_check.cmake test nm's both and asserts the dev filesystem
// watcher symbol is present in dev and ABSENT in ship — proving a shipping build
// carries no filesystem-watch / auto-dlopen entry point.
#include <pulp/format/reload/reloadable_shell.hpp>

int main() {
    pulp::format::reload::ReloadableShell shell;  // force class emission
    (void)shell.descriptor();
    return 0;
}
