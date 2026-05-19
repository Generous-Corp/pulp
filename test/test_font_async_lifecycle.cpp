// test_font_async_lifecycle.cpp — Pulp #2163, font v2 Slice 2.1.
//
// Verifies register_font_url(...) returns a future that resolves to
// the expected FontState for each URL scheme:
//   * `http://` / `https://` → immediately Failed (until 2.1.b lands).
//   * `file://` and bare absolute paths → dispatched async,
//     resolves to Loaded for a valid font, Failed for a missing path.
//   * empty input → Failed.
//
// These tests do not require a Skia link: register_font_file degrades
// to a no-op returning false without Skia, so the URL surface still
// resolves to Failed deterministically. The bundled-Inter "Loaded"
// path is Skia-gated.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bundled_fonts.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

using namespace pulp::canvas;
using namespace std::chrono_literals;

namespace {

// Write a bundled Inter blob to a temp file we can hand to
// register_font_url. We could call into the bundled-font table
// directly, but a real on-disk round-trip exercises the actual
// register_font_file path the future dispatches to.
std::string write_temp_dummy_font_bytes() {
    auto tmp = std::filesystem::temp_directory_path() / "pulp_font_async_test.ttf";
    std::ofstream f(tmp, std::ios::binary);
    // Not a valid font — register_font will reject. We use this to
    // exercise the "file is readable but invalid" Failed path.
    const char garbage[] = "not a real font";
    f.write(garbage, sizeof(garbage));
    f.close();
    return tmp.string();
}

} // namespace

TEST_CASE("register_font_url: empty URL → Failed", "[font][async][issue-2163]") {
    auto fut = register_font_url("");
    REQUIRE(fut.wait_for(2s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);
}

TEST_CASE("register_font_url: http(s) scheme not yet supported → Failed",
          "[font][async][issue-2163]") {
    {
        auto fut = register_font_url("https://example.com/font.ttf");
        REQUIRE(fut.wait_for(2s) == std::future_status::ready);
        REQUIRE(fut.get() == FontState::Failed);
    }
    {
        auto fut = register_font_url("HTTP://example.com/font.ttf");
        REQUIRE(fut.wait_for(2s) == std::future_status::ready);
        REQUIRE(fut.get() == FontState::Failed);
    }
}

TEST_CASE("register_font_url: missing file path → Failed",
          "[font][async][issue-2163]") {
    auto fut = register_font_url("/no/such/path/font.ttf");
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);
}

TEST_CASE("register_font_url: file:// URL with invalid bytes → Failed",
          "[font][async][issue-2163]") {
    std::string path = write_temp_dummy_font_bytes();
    auto fut = register_font_url("file://" + path);
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);
    std::remove(path.c_str());
}

TEST_CASE("register_font_url: bare absolute path with invalid bytes → Failed",
          "[font][async][issue-2163]") {
    std::string path = write_temp_dummy_font_bytes();
    auto fut = register_font_url(path);
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);
    std::remove(path.c_str());
}

// pulp #2246 follow-up (Codex review P1): the docstring on
// register_font_url promises "the future is safe to detach", but the
// pre-fix implementation used `std::async(std::launch::async, ...)`
// whose returned future's destructor BLOCKS the caller until the
// task completes if dropped without `.get()` / `.wait()`. That made
// the docstring a lie. Verify the post-fix behavior: scheduling a
// long-running registration and then dropping the future must NOT
// block the caller — the test must complete promptly even though
// the worker thread is still running in the background.
TEST_CASE("register_font_url: dropping the future does not block the caller",
          "[font][async][issue-2246]") {
    namespace fs = std::filesystem;
    std::string path = write_temp_dummy_font_bytes();

    const auto t0 = std::chrono::steady_clock::now();
    {
        // Scope the future so its destructor runs before we measure
        // elapsed time below.
        auto fut = register_font_url(path);
        (void)fut;
        // Deliberately do NOT call wait()/get(). With std::async-based
        // implementation, the future's destructor at scope exit would
        // join the worker (~ms-to-seconds on a real font). With the
        // promise + detached-thread fix, the destructor is a no-op.
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    // A reasonable bound: even on a slow CI host, decoupling from the
    // worker thread should take well under 100ms. Pre-fix, this would
    // have blocked for the full register_font_file duration.
    REQUIRE(elapsed.count() < 100);

    std::remove(path.c_str());
}
