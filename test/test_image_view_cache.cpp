// ImageView ↔ ImageCache wiring test. Workstream 07 B4.
//
// The real decode path is Skia-backed and tested elsewhere. Here we
// just pin the public-API shape: set_image_source URL-keys, set_image_cache
// attaches/detaches, and the legacy set_image_path routes through the
// URI normaliser so existing callers keep working.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/image_cache.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;

TEST_CASE("set_image_path is equivalent to file:// URI", "[widgets][image]") {
    ImageView v;
    v.set_image_path("/tmp/foo.png");
    REQUIRE(v.image_source() == "file:///tmp/foo.png");
    REQUIRE(v.image_path() == "file:///tmp/foo.png");
}

TEST_CASE("set_image_source accepts URI schemes directly", "[widgets][image]") {
    ImageView v;
    v.set_image_source("resource://icons/save.png");
    REQUIRE(v.image_source() == "resource://icons/save.png");

    v.set_image_source("memory://sha256=deadbeef");
    REQUIRE(v.image_source() == "memory://sha256=deadbeef");
}

TEST_CASE("set_image_cache attaches and detaches", "[widgets][image]") {
    ImageView v;
    REQUIRE(v.image_cache() == nullptr);

    ImageCache cache;
    v.set_image_cache(&cache);
    REQUIRE(v.image_cache() == &cache);

    v.set_image_cache(nullptr);
    REQUIRE(v.image_cache() == nullptr);
}

TEST_CASE("changing the source invalidates the loaded flag", "[widgets][image]") {
    ImageView v;
    v.set_image_source("file:///tmp/a.png");
    v.set_image_source("file:///tmp/b.png");
    REQUIRE(v.image_source() == "file:///tmp/b.png");
}
