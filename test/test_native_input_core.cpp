// The parts of pulp::native that name no platform event and are built on every
// platform: key names, the key routing rule, GL request selection and the
// software-renderer test. The AppKit translation entry points answer "nothing
// translated" off macOS.

#include <pulp/native/gl_surface.hpp>
#include <pulp/native/input.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace pulp::native;

TEST_CASE("key names are stable and cover the whole enum", "[native][input]") {
    REQUIRE(key_name(Key::unknown) == "unknown");
    REQUIRE(key_name(Key::a) == "a");
    REQUIRE(key_name(Key::space) == "space");
    REQUIRE(key_name(Key::f13) == "f13");
    REQUIRE(key_name(Key::keypad_enter) == "keypad_enter");
    REQUIRE(key_name(Key::jis_kana) == "jis_kana");
    // A value past the last key is "unknown", never an out-of-range read.
    REQUIRE(key_name(static_cast<Key>(60000)) == "unknown");
}

TEST_CASE("the host keeps every key unless the client holds the keyboard", "[native][input]") {
    KeyEvent space;
    space.key = Key::space;
    REQUIRE(route_key(space, KeyboardFocus::none) == KeyRoute::host);
    REQUIRE(route_key(space, KeyboardFocus::keys) == KeyRoute::client_first);
    REQUIRE(route_key(space, KeyboardFocus::text) == KeyRoute::client_first);
    REQUIRE(route_key(space, false) == KeyRoute::host);
    REQUIRE(route_key(space, true) == KeyRoute::client_first);
}

TEST_CASE("GL request selection needs the version, profile and samples", "[native][gl-surface]") {
    const std::vector<GlContextRequest> list{
        {4, 6, GlProfile::core, 0}, {3, 3, GlProfile::core, 4}, {2, 1, GlProfile::any, 0}};
    REQUIRE(first_honoured_request(list, 4, 6, GlProfile::core) == 0u);
    REQUIRE(first_honoured_request(list, 3, 3, GlProfile::core) == 1u);
    REQUIRE(first_honoured_request(list, 3, 3, GlProfile::core, 0) == 2u);
    REQUIRE(first_honoured_request(list, 3, 3, GlProfile::core, 4) == 1u);
    REQUIRE_FALSE(first_honoured_request({{4, 6, GlProfile::core, 0}}, 3, 3, GlProfile::core));
}

TEST_CASE("software renderers are recognised by name", "[native][gl-surface]") {
    REQUIRE(is_software_renderer("llvmpipe (LLVM 15.0.7, 128 bits)"));
    REQUIRE(is_software_renderer("Software Rasterizer swrast"));
    REQUIRE(is_software_renderer("GDI Generic"));
    REQUIRE_FALSE(is_software_renderer("NVIDIA GeForce RTX 4090/PCIe/SSE2"));
    REQUIRE_FALSE(is_software_renderer(""));
}

#if !defined(__APPLE__)
TEST_CASE("the AppKit translation reports nothing translated off macOS", "[native][input]") {
    REQUIRE_FALSE(translate_pointer(nullptr, nullptr).has_value());
    REQUIRE_FALSE(translate_wheel(nullptr, nullptr).has_value());
    REQUIRE_FALSE(translate_key(nullptr).has_value());
    REQUIRE_FALSE(translate_flags_changed(nullptr).has_value());
    REQUIRE(ns_cursor_for(Cursor::arrow) == nullptr);
    REQUIRE_FALSE(cursor_is_native(Cursor::arrow));
    REQUIRE(key_for_native_code(49) == Key::unknown);
    REQUIRE(modifiers_from_ns_flags(~0ull) == 0u);
}
#endif
