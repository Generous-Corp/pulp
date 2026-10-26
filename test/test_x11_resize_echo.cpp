// X11ResizeEcho: a ConfigureNotify older than the editor's last XResizeWindow
// is a stale echo and must not put the editor back to an earlier size. Pure
// serial arithmetic, so it runs on every platform.

#include <catch2/catch_test_macros.hpp>

#include "../core/view/platform/linux/x11_resize_echo.hpp"

#include <climits>

using pulp::view::X11ResizeEcho;

TEST_CASE("X11 resize echo accepts every event before any self resize", "[x11-resize-echo]") {
    X11ResizeEcho echo;
    CHECK_FALSE(echo.is_stale(0));
    CHECK_FALSE(echo.is_stale(12345));
}

TEST_CASE("X11 resize echo drops an echo of an earlier self resize", "[x11-resize-echo]") {
    X11ResizeEcho echo;
    echo.note_self_resize(100); // set_size(300): XResizeWindow is request 100
    echo.note_self_resize(140); // set_size(400): XResizeWindow is request 140
    // The echo of the first resize carries serial 100: stale, ignored.
    CHECK(echo.is_stale(100));
    CHECK(echo.is_stale(139));
    // The second resize's own echo, and anything the host did after it, count.
    CHECK_FALSE(echo.is_stale(140));
    CHECK_FALSE(echo.is_stale(141));
}

TEST_CASE("X11 resize echo compares serials across wraparound", "[x11-resize-echo]") {
    X11ResizeEcho echo;
    echo.note_self_resize(2);            // the request counter just wrapped
    CHECK(echo.is_stale(ULONG_MAX - 5)); // issued before the wrap
    CHECK_FALSE(echo.is_stale(2));
    CHECK_FALSE(echo.is_stale(7));
}
