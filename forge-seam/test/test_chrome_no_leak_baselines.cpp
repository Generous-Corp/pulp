// Private translation unit for the pixel-exact Chrome no-leak guard.

// The no-leak guard: a change made for one Forge product must not alter another.
//
// Forge varies ONE chrome across three products through ShellKind. That sharing
// is the point -- it is what keeps the products looking like one family -- but it
// also means a change intended for one can silently move another. Renders exist
// in test_chrome.cpp today; nothing compares them to anything, so a drift is
// invisible until somebody notices by eye.
//
// This renders each product's Home frame and compares it to a committed
// baseline. Adding a fourth product, or touching shared chrome for any reason,
// must leave these three byte-identical.
//
// Refresh a baseline deliberately, never casually:
//     FORGE_NO_LEAK_UPDATE=1 ./forge-test-chrome-no-leak
// and commit the changed PNGs with the reason in the message.

#include "chrome_no_leak_test_support.hpp"

TEST_CASE("Forge FX's Home frame matches its baseline", "[no-leak]") {
    check_home_frame<forge::ForgeFxShell>("fx");
}

TEST_CASE("Forge Instrument's Home frame matches its baseline", "[no-leak]") {
    check_home_frame<forge::ForgeInstrumentShell>("instrument");
}

TEST_CASE("Forge MIDI's Home frame matches its baseline", "[no-leak]") {
    // MIDI ships CLAP and AU only -- no standalone to screenshot -- which is
    // exactly why this guard renders the chrome directly instead of driving three
    // apps. Every product is covered whether or not it has a window.
    check_home_frame<forge::ForgeMidiShell>("midi");
}
