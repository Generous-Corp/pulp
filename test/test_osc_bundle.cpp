#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/osc/bundle.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace pulp::osc;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<uint8_t> make_empty_bundle_bytes() {
    std::vector<uint8_t> buf;
    const char header[] = "#bundle";
    buf.insert(buf.end(), header, header + 8);
    for (int i = 0; i < 8; ++i) buf.push_back(0);
    return buf;
}

void append_u32(std::vector<uint8_t>& buf, uint32_t value) {
    uint32_t net = htonl(value);
    auto* p = reinterpret_cast<const uint8_t*>(&net);
    buf.insert(buf.end(), p, p + 4);
}

}  // namespace

// ── TimeTag ─────────────────────────────────────────────────────────────

TEST_CASE("OSC TimeTag immediate", "[osc][bundle]") {
    auto tt = TimeTag::immediate();
    REQUIRE(tt.seconds == 0);
    REQUIRE(tt.fraction == 1);
}

TEST_CASE("OSC TimeTag from/to unix round-trip", "[osc][bundle]") {
    double unix_time = 1700000000.5;  // 2023-11-14 approx
    auto tt = TimeTag::from_unix(unix_time);
    REQUIRE(tt.seconds > 0);  // NTP epoch is 1900, so seconds > unix seconds
    REQUIRE_THAT(tt.to_unix(), WithinAbs(unix_time, 0.001));
}

TEST_CASE("OSC TimeTag equality", "[osc][bundle]") {
    auto a = TimeTag::from_unix(1000000.0);
    auto b = TimeTag::from_unix(1000000.0);
    REQUIRE(a == b);
}

// ── Bundle construction ─────────────────────────────────────────────────

TEST_CASE("OSC Bundle add message", "[osc][bundle]") {
    Bundle bundle;
    Message msg;
    msg.address = "/test";
    msg.add(42);
    bundle.add(std::move(msg));

    REQUIRE(bundle.elements.size() == 1);
    REQUIRE(bundle.elements[0].is_message());
    REQUIRE(bundle.elements[0].message().address == "/test");
}

TEST_CASE("OSC Bundle add nested bundle", "[osc][bundle]") {
    Bundle inner;
    inner.timetag = TimeTag::from_unix(100.0);
    Message msg;
    msg.address = "/inner";
    inner.add(std::move(msg));

    Bundle outer;
    outer.add(std::move(inner));

    REQUIRE(outer.elements.size() == 1);
    REQUIRE(outer.elements[0].is_bundle());
    REQUIRE(outer.elements[0].bundle().elements.size() == 1);
}

// ── Bundle serialization ────────────────────────────────────────────────

TEST_CASE("OSC Bundle serialize produces valid data", "[osc][bundle]") {
    Bundle bundle;
    bundle.timetag = TimeTag::immediate();

    Message msg;
    msg.address = "/gain";
    msg.add(0.75f);
    bundle.add(std::move(msg));

    auto data = bundle.serialize();
    REQUIRE(data.size() > 8);  // At minimum: #bundle + timetag

    // Check #bundle header (8 bytes: "#bundle" + null terminator)
    REQUIRE(data[0] == '#');
    REQUIRE(data[1] == 'b');
    REQUIRE(data[6] == 'e');
    REQUIRE(data[7] == '\0');
}

TEST_CASE("OSC Bundle serialize/deserialize round-trip", "[osc][bundle]") {
    Bundle original;
    original.timetag = TimeTag::from_unix(1700000000.0);

    Message msg1;
    msg1.address = "/volume";
    msg1.add(-6.0f);
    original.add(std::move(msg1));

    Message msg2;
    msg2.address = "/mute";
    msg2.add(1);
    original.add(std::move(msg2));

    auto data = original.serialize();
    REQUIRE(data.size() > 0);

    auto restored = Bundle::deserialize(data.data(), data.size());
    REQUIRE(restored.has_value());
    REQUIRE(restored->timetag == original.timetag);
    REQUIRE(restored->elements.size() == 2);
    REQUIRE(restored->elements[0].is_message());
    REQUIRE(restored->elements[0].message().address == "/volume");
}

// ── Address pattern matching ────────────────────────────────────────────

TEST_CASE("OSC address exact match", "[osc][bundle]") {
    REQUIRE(address_matches("/foo/bar", "/foo/bar"));
    REQUIRE_FALSE(address_matches("/foo/bar", "/foo/baz"));
}

TEST_CASE("OSC address wildcard *", "[osc][bundle]") {
    REQUIRE(address_matches("/foo/*", "/foo/bar"));
    REQUIRE(address_matches("/*/bar", "/foo/bar"));
    REQUIRE(address_matches("/*", "/anything"));
}

TEST_CASE("OSC address wildcard ?", "[osc][bundle]") {
    REQUIRE(address_matches("/foo/ba?", "/foo/bar"));
    REQUIRE(address_matches("/foo/ba?", "/foo/baz"));
    REQUIRE_FALSE(address_matches("/foo/ba?", "/foo/ba"));
}

// ── TimeTag edges ──────────────────────────────────────────────────────

TEST_CASE("TimeTag::from_unix with negative time returns immediate",
          "[osc][bundle][timetag]") {
    auto tt = TimeTag::from_unix(-1.0);
    REQUIRE(tt == TimeTag::immediate());
}

TEST_CASE("TimeTag::from_unix with zero returns immediate",
          "[osc][bundle][timetag]") {
    auto tt = TimeTag::from_unix(0.0);
    REQUIRE(tt == TimeTag::immediate());
}

TEST_CASE("TimeTag inequality on different fractional components",
          "[osc][bundle][timetag]") {
    TimeTag a{100, 0};
    TimeTag b{100, 1};
    REQUIRE_FALSE(a == b);
}

// ── Bundle deserialize rejection paths ─────────────────────────────────

TEST_CASE("Bundle::deserialize rejects data shorter than 16 bytes",
          "[osc][bundle][deserialize-edge]") {
    const uint8_t tiny[] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0};
    REQUIRE_FALSE(Bundle::deserialize(tiny, sizeof(tiny)).has_value());
}

TEST_CASE("Bundle::deserialize rejects non-#bundle header",
          "[osc][bundle][deserialize-edge]") {
    // 16 bytes, valid size, but first 8 bytes aren't "#bundle\0".
    const uint8_t bad[16] = {
        'X', 'b', 'u', 'n', 'd', 'l', 'e', 0,
        0, 0, 0, 0, 0, 0, 0, 1
    };
    REQUIRE_FALSE(Bundle::deserialize(bad, sizeof(bad)).has_value());
}

TEST_CASE("Bundle::deserialize rejects element size that walks past end",
          "[osc][bundle][deserialize-edge]") {
    // Header + timetag (16 bytes) + element size prefix (4 bytes) claiming
    // 64 payload bytes, but only 4 actual bytes follow. Deserialize must
    // bail out rather than reading past the end.
    std::vector<uint8_t> buf;
    const char header[] = "#bundle";
    buf.insert(buf.end(), header, header + 8);
    for (int i = 0; i < 8; ++i) buf.push_back(0);
    uint32_t size_claim = htonl(64);
    auto* p = reinterpret_cast<const uint8_t*>(&size_claim);
    buf.insert(buf.end(), p, p + 4);
    buf.insert(buf.end(), {0xAA, 0xBB, 0xCC, 0xDD});

    REQUIRE_FALSE(Bundle::deserialize(buf.data(), buf.size()).has_value());
}

TEST_CASE("Bundle::deserialize rejects trailing short bytes after bundle body",
          "[osc][bundle][deserialize-edge]") {
    auto buf = make_empty_bundle_bytes();
    buf.push_back(0x12);
    buf.push_back(0x34);
    buf.push_back(0x56);

    REQUIRE_FALSE(Bundle::deserialize(buf.data(), buf.size()).has_value());
}

TEST_CASE("Bundle::deserialize rejects malformed nested bundle element",
          "[osc][bundle][deserialize-edge]") {
    auto buf = make_empty_bundle_bytes();

    std::vector<uint8_t> nested = {
        '#', 'b', 'u', 'n', 'd', 'l', 'e', 0,
        0, 0, 0, 0, 0, 0, 0, 1
    };
    nested.push_back(0xFF);  // trailing short byte in nested payload

    append_u32(buf, static_cast<uint32_t>(nested.size()));
    buf.insert(buf.end(), nested.begin(), nested.end());

    REQUIRE_FALSE(Bundle::deserialize(buf.data(), buf.size()).has_value());
}

TEST_CASE("Bundle::deserialize rejects malformed message element",
          "[osc][bundle][deserialize-edge]") {
    auto buf = make_empty_bundle_bytes();

    const std::vector<uint8_t> malformed_msg = {
        'n', 'o', 's', 'l', 'a', 's', 'h', 0,
        ',', 0, 0, 0
    };

    append_u32(buf, static_cast<uint32_t>(malformed_msg.size()));
    buf.insert(buf.end(), malformed_msg.begin(), malformed_msg.end());

    REQUIRE_FALSE(Bundle::deserialize(buf.data(), buf.size()).has_value());
}

// ── Bundle round-trip edges ────────────────────────────────────────────

TEST_CASE("Empty Bundle serialize/deserialize round-trip",
          "[osc][bundle][roundtrip]") {
    Bundle b;
    b.timetag = {0, 1};  // immediate
    auto data = b.serialize();
    REQUIRE(data.size() == 16);  // header(8) + timetag(8), no elements

    auto restored = Bundle::deserialize(data.data(), data.size());
    REQUIRE(restored.has_value());
    REQUIRE(restored->timetag == b.timetag);
    REQUIRE(restored->elements.empty());
}

TEST_CASE("Bundle with mixed messages and nested bundles round-trips",
          "[osc][bundle][roundtrip]") {
    Bundle inner;
    inner.timetag = TimeTag::from_unix(1700000001.0);
    Message inner_msg("/inner");
    inner_msg.add(7);
    inner.add(std::move(inner_msg));

    Bundle outer;
    outer.timetag = TimeTag::from_unix(1700000000.0);
    Message m1("/a"); m1.add(1);
    Message m2("/b"); m2.add(std::string("two"));
    outer.add(std::move(m1));
    outer.add(std::move(inner));
    outer.add(std::move(m2));

    auto data = outer.serialize();
    auto restored = Bundle::deserialize(data.data(), data.size());
    REQUIRE(restored.has_value());
    REQUIRE(restored->elements.size() == 3);
    REQUIRE(restored->elements[0].is_message());
    REQUIRE(restored->elements[1].is_bundle());
    REQUIRE(restored->elements[2].is_message());
    REQUIRE(restored->elements[0].message().address == "/a");
    REQUIRE(restored->elements[1].bundle().elements.size() == 1);
    REQUIRE(restored->elements[1].bundle().elements[0].message().address == "/inner");
    REQUIRE(restored->elements[2].message().get_string(0) == "two");
}

TEST_CASE("Bundle deep nesting (3 levels) round-trips",
          "[osc][bundle][roundtrip]") {
    Bundle level3;
    Message leaf("/leaf"); leaf.add(42);
    level3.add(std::move(leaf));

    Bundle level2;
    level2.add(std::move(level3));

    Bundle level1;
    level1.add(std::move(level2));

    auto data = level1.serialize();
    auto restored = Bundle::deserialize(data.data(), data.size());
    REQUIRE(restored.has_value());
    REQUIRE(restored->elements.size() == 1);
    REQUIRE(restored->elements[0].is_bundle());
    const auto& l2 = restored->elements[0].bundle();
    REQUIRE(l2.elements[0].is_bundle());
    const auto& l3 = l2.elements[0].bundle();
    REQUIRE(l3.elements[0].is_message());
    REQUIRE(l3.elements[0].message().get_int(0) == 42);
}

// ── Address pattern: character class ───────────────────────────────────

TEST_CASE("Address pattern character class range [a-z]",
          "[osc][bundle][pattern]") {
    REQUIRE(address_matches("/note/[a-z]", "/note/c"));
    REQUIRE(address_matches("/note/[a-z]", "/note/z"));
    REQUIRE_FALSE(address_matches("/note/[a-z]", "/note/A"));
    REQUIRE_FALSE(address_matches("/note/[a-z]", "/note/1"));
}

TEST_CASE("Address pattern character class enumeration [abc]",
          "[osc][bundle][pattern]") {
    REQUIRE(address_matches("/pad/[xyz]", "/pad/y"));
    REQUIRE_FALSE(address_matches("/pad/[xyz]", "/pad/w"));
}

TEST_CASE("Address pattern character class negation [!...]",
          "[osc][bundle][pattern]") {
    REQUIRE(address_matches("/ch/[!0-9]", "/ch/a"));
    REQUIRE_FALSE(address_matches("/ch/[!0-9]", "/ch/5"));
}

// ── Address pattern: alternatives ──────────────────────────────────────

TEST_CASE("Address pattern alternatives {foo,bar}",
          "[osc][bundle][pattern]") {
    REQUIRE(address_matches("/{in,out}/gain", "/in/gain"));
    REQUIRE(address_matches("/{in,out}/gain", "/out/gain"));
    REQUIRE_FALSE(address_matches("/{in,out}/gain", "/mid/gain"));
}

TEST_CASE("Address pattern alternatives with trailing literal",
          "[osc][bundle][pattern]") {
    REQUIRE(address_matches("/{f,bar}oo", "/foo"));
    REQUIRE(address_matches("/{f,bar}oo", "/baroo"));
    REQUIRE_FALSE(address_matches("/{f,bar}oo", "/bazoo"));
}

// ── Address pattern: lengths & bounds ──────────────────────────────────

TEST_CASE("Address pattern pattern shorter than address fails",
          "[osc][bundle][pattern]") {
    REQUIRE_FALSE(address_matches("/foo", "/foo/bar"));
}

TEST_CASE("Address pattern pattern longer than address fails",
          "[osc][bundle][pattern]") {
    REQUIRE_FALSE(address_matches("/foo/bar", "/foo"));
}

TEST_CASE("Address pattern star bounded by path separator",
          "[osc][bundle][pattern]") {
    // '*' matches any run of characters up to the next '/', not across.
    REQUIRE(address_matches("/*/bar", "/any/bar"));
    REQUIRE_FALSE(address_matches("/*/bar", "/any/thing/bar"));
}

TEST_CASE("Malformed address patterns fail closed",
          "[osc][bundle][pattern]") {
    REQUIRE_FALSE(address_matches("/note/[ab", "/note/a"));
    REQUIRE_FALSE(address_matches("/note/[!0-9", "/note/a"));
    REQUIRE_FALSE(address_matches("/{foo,bar/gain", "/foo/gain"));
}
