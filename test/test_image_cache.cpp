// ImageCache tests (workstream 07 slice 7.4).

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/image_cache.hpp>

#include <atomic>
#include <cstdint>

using namespace pulp::view;

namespace {

// Fake decoder: maps URI to a fixed DecodedImage keyed by length so
// tests produce deterministic sizes.
ImageDecodeFn make_fake_decoder(std::atomic<int>& decode_calls) {
    return [&](const std::string& uri) -> std::optional<DecodedImage> {
        ++decode_calls;
        DecodedImage img;
        img.width = 4;
        img.height = 4;
        img.native_handle = reinterpret_cast<void*>(uri.size() + 0x1000);
        img.bytes = img.width * img.height * 4;  // 64
        return img;
    };
}

} // namespace

TEST_CASE("miss then hit", "[view][image-cache]") {
    ImageCache c;
    std::atomic<int> calls{0};
    c.set_decoder(make_fake_decoder(calls));

    auto* a = c.get("a");
    REQUIRE(a != nullptr);
    REQUIRE(c.stats().misses == 1);

    auto* b = c.get("a");
    REQUIRE(b == a);
    REQUIRE(c.stats().hits == 1);
    REQUIRE(calls.load() == 1);
}

TEST_CASE("no-decoder returns nullptr", "[view][image-cache]") {
    ImageCache c;
    REQUIRE(c.get("a") == nullptr);
    REQUIRE(c.stats().misses == 1);
}

TEST_CASE("decode failure is not cached", "[view][image-cache]") {
    ImageCache c;
    std::atomic<int> calls{0};
    c.set_decoder([&](const std::string&) -> std::optional<DecodedImage> {
        ++calls;
        return std::nullopt;
    });

    REQUIRE(c.get("missing") == nullptr);
    REQUIRE(c.get("missing") == nullptr);

    auto s = c.stats();
    REQUIRE(calls.load() == 2);
    REQUIRE(s.misses == 2);
    REQUIRE(s.hits == 0);
    REQUIRE(s.entry_count == 0);
    REQUIRE(s.total_bytes == 0);
}

TEST_CASE("LRU eviction honours byte budget", "[view][image-cache]") {
    ImageCache c;
    std::atomic<int> calls{0};
    c.set_decoder(make_fake_decoder(calls));
    c.set_byte_budget(64 * 2);   // fits exactly 2 entries (64 each)

    c.get("a"); c.get("b");
    REQUIRE(c.stats().entry_count == 2);

    c.get("c");  // triggers eviction of LRU (a)
    auto s = c.stats();
    REQUIRE(s.entry_count == 2);
    REQUIRE(s.evictions >= 1);

    // b was used after a, so a is the LRU victim. c and b remain.
    calls = 0;
    c.get("b");
    REQUIRE(calls.load() == 0);   // hit
    c.get("a");
    REQUIRE(calls.load() == 1);   // miss → re-decoded
}

TEST_CASE("entry larger than byte budget is discarded", "[view][image-cache]") {
    ImageCache c;
    std::atomic<int> calls{0};
    std::atomic<int> released{0};
    c.set_decoder(make_fake_decoder(calls));
    c.set_releaser([&](DecodedImage&) { ++released; });
    c.set_byte_budget(63);  // fake images are 64 bytes each

    REQUIRE(c.get("too-large") == nullptr);

    auto s = c.stats();
    REQUIRE(calls.load() == 1);
    REQUIRE(released.load() == 1);
    REQUIRE(s.misses == 1);
    REQUIRE(s.entry_count == 0);
    REQUIRE(s.total_bytes == 0);
    REQUIRE(s.evictions == 1);
}

TEST_CASE("clear invokes releaser for every entry",
          "[view][image-cache]") {
    ImageCache c;
    std::atomic<int> calls{0};
    std::atomic<int> released{0};
    c.set_decoder(make_fake_decoder(calls));
    c.set_releaser([&](DecodedImage&) { ++released; });

    c.get("a");
    c.get("b");
    c.get("c");
    REQUIRE(c.stats().entry_count == 3);

    c.clear();
    REQUIRE(released.load() == 3);
    REQUIRE(c.stats().entry_count == 0);
    REQUIRE(c.stats().total_bytes == 0);
}

TEST_CASE("zero budget disables trimming", "[view][image-cache]") {
    ImageCache c;
    std::atomic<int> calls{0};
    c.set_decoder(make_fake_decoder(calls));
    for (int i = 0; i < 100; ++i) c.get("u" + std::to_string(i));
    REQUIRE(c.stats().entry_count == 100);
    REQUIRE(c.stats().evictions == 0);
}

TEST_CASE("stats track hits/misses/evictions",
          "[view][image-cache]") {
    ImageCache c;
    std::atomic<int> calls{0};
    c.set_decoder(make_fake_decoder(calls));
    c.set_byte_budget(64);  // one entry at a time

    c.get("a");
    c.get("b");  // evicts a
    c.get("a");  // misses again (evicted), evicts b

    auto s = c.stats();
    REQUIRE(s.misses == 3);
    REQUIRE(s.hits == 0);
    REQUIRE(s.evictions >= 2);
    REQUIRE(s.entry_count == 1);
}
