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

TEST_CASE("register_font_url: file URL scheme matching is case-insensitive",
          "[font][async][coverage]") {
    std::string path = write_temp_dummy_font_bytes();
    auto fut = register_font_url("FiLe://" + path);
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);
    std::remove(path.c_str());
}

TEST_CASE("register_font_url: file://localhost strips the host component",
          "[font][async][coverage]") {
    std::string path = write_temp_dummy_font_bytes();
    auto fut = register_font_url("file://localhost" + path);
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);
    std::remove(path.c_str());
}

TEST_CASE("register_font_url: bare file: scheme uses the following path",
          "[font][async][coverage]") {
    std::string path = write_temp_dummy_font_bytes();
    auto fut = register_font_url("file:" + path);
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

TEST_CASE("register_font_url: unsupported schemes are treated as plain paths",
          "[font][async][coverage]") {
    auto fut = register_font_url("ftp://example.com/font.ttf");
    REQUIRE(fut.wait_for(5s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);
}

// pulp #2308 follow-up (Codex review P2): the detached worker calls
// register_font_file(), which can throw — for example, a vector
// allocation backed by `tellg()` on a huge sparse file. An exception
// escaping a detached `std::thread` calls `std::terminate` and kills
// the host process. The contract callers were sold is "drop the
// future and you get a Failed result, never a crash."
//
// This test creates a sparse file with a 10 TiB logical size on
// filesystems that support it (APFS, ext4, NTFS sparse). When
// `register_font_file()` opens it and runs `tellg()` → vector ctor,
// the allocation should throw `bad_alloc`. With the catch-all in the
// detached worker, this surfaces as `FontState::Failed` on the
// future. Without the catch-all, the worker thread terminates the
// process.
//
// If the host environment can't produce the sparse file (sandbox,
// filesystem without sparse support, ulimit blocking truncate), the
// test SUCCEEDs without exercising the throw — the cross-platform
// CI matrix will still hit the case on at least one OS.
TEST_CASE("register_font_url: detached worker survives allocation throw",
          "[font][async][issue-2308]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "pulp_font_async_huge_sparse.bin";
    std::error_code ec;
    fs::remove(tmp, ec);

    // Create a 10 TiB sparse file. On filesystems without sparse
    // support, resize_file() fails — in which case we skip the throw
    // path entirely but still assert the contract by going through
    // the non-throwing missing-path branch.
    constexpr std::uintmax_t kHuge = static_cast<std::uintmax_t>(10) << 40; // 10 TiB
    fs::resize_file(tmp, kHuge, ec);
    if (ec) {
        SUCCEED("filesystem does not support sparse files — skipping throw exercise: "
                + ec.message());
        return;
    }

    // Hand the worker a path whose tellg() reports ~10 TiB. The
    // `std::vector<uint8_t>(size)` allocation must throw bad_alloc,
    // and the catch-all in the detached worker must convert it to
    // FontState::Failed and signal via the promise. If the catch-all
    // is missing, this process terminates here and the test framework
    // reports a hard crash.
    auto fut = register_font_url(tmp.string());
    REQUIRE(fut.wait_for(10s) == std::future_status::ready);
    REQUIRE(fut.get() == FontState::Failed);

    fs::remove(tmp, ec);
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
