// Tests for the self-describing EDITMODE parameter block: locate, read, rewrite.

#include <pulp/design/design_tweaks.hpp>

#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

#include <string>

using namespace pulp::design;

namespace {

// An artifact with surrounding content the rewriter must never disturb.
const std::string kArtifact =
    "export const ui = () => {\n"
    "  const p = /*EDITMODE-BEGIN*/{\"accent\":\"#33aaff\",\"radius\":8}/*EDITMODE-END*/;\n"
    "  return render(p);\n"
    "};\n";

std::string param_value(const std::vector<TweakParam>& ps, const std::string& key) {
    for (const auto& p : ps)
        if (p.key == key) return p.json_value;
    return "<absent>";
}

}  // namespace

TEST_CASE("find_edit_block locates the marker span", "[tweaks]") {
    auto span = find_edit_block(kArtifact);
    REQUIRE(span.has_value());
    CHECK(kArtifact.substr(span->block_begin,
                           std::string_view(kEditBlockBegin).size()) ==
          std::string(kEditBlockBegin));
    CHECK(kArtifact.substr(span->payload_begin, 1) == "{");
    CHECK(kArtifact.substr(span->payload_end - 1, 1) == "}");
    // block_end points just past the closing marker.
    CHECK(kArtifact.substr(span->block_end - std::string_view(kEditBlockEnd).size(),
                           std::string_view(kEditBlockEnd).size()) ==
          std::string(kEditBlockEnd));
}

TEST_CASE("find_edit_block returns nullopt with no markers", "[tweaks]") {
    CHECK_FALSE(find_edit_block("no block here").has_value());
}

TEST_CASE("find_edit_block treats an unterminated block as absent", "[tweaks]") {
    // Opening marker but no close -> must not span to EOF.
    CHECK_FALSE(find_edit_block("a /*EDITMODE-BEGIN*/{\"x\":1} still open").has_value());
}

TEST_CASE("read_edit_block parses params in member order", "[tweaks]") {
    auto ps = read_edit_block(kArtifact);
    REQUIRE(ps.has_value());
    REQUIRE(ps->size() == 2);
    CHECK((*ps)[0].key == "accent");
    CHECK((*ps)[0].json_value == "\"#33aaff\"");  // value keeps its JSON quoting
    CHECK((*ps)[1].key == "radius");
    CHECK((*ps)[1].json_value == "8");
}

TEST_CASE("find_edit_block rejects trailing bytes after the object", "[tweaks]") {
    // `{...};evil()` between the markers must not be accepted as `{...}` and then
    // silently dropped by a later rewrite — the block is malformed.
    CHECK_FALSE(find_edit_block("/*EDITMODE-BEGIN*/{\"a\":1};evil()/*EDITMODE-END*/").has_value());
    CHECK_FALSE(read_edit_block("/*EDITMODE-BEGIN*/{\"a\":1} junk/*EDITMODE-END*/").has_value());
}

TEST_CASE("find_edit_block ignores an end-marker inside a JSON string", "[tweaks]") {
    // A value that literally contains the end-marker text must not truncate the
    // block; the real closing marker is the one outside any string.
    auto ps = read_edit_block(
        "/*EDITMODE-BEGIN*/{\"label\":\"x /*EDITMODE-END*/ y\"}/*EDITMODE-END*/");
    REQUIRE(ps.has_value());
    REQUIRE(ps->size() == 1);
    CHECK((*ps)[0].key == "label");
    CHECK((*ps)[0].json_value == "\"x /*EDITMODE-END*/ y\"");
}

TEST_CASE("find_edit_block ignores a stray end-marker before any begin", "[tweaks]") {
    // A close-before-open ordering is not a block.
    CHECK_FALSE(find_edit_block("/*EDITMODE-END*/ noise").has_value());
    // But a well-formed block after stray content is still found.
    auto span = find_edit_block("/*EDITMODE-END*/ x /*EDITMODE-BEGIN*/{\"a\":1}/*EDITMODE-END*/");
    REQUIRE(span.has_value());
}

TEST_CASE("read_edit_block rejects a non-object payload", "[tweaks]") {
    CHECK_FALSE(read_edit_block("/*EDITMODE-BEGIN*/[1,2,3]/*EDITMODE-END*/").has_value());
}

TEST_CASE("read_edit_block rejects malformed JSON", "[tweaks]") {
    CHECK_FALSE(read_edit_block("/*EDITMODE-BEGIN*/{bad json/*EDITMODE-END*/").has_value());
}

TEST_CASE("read_edit_block rejects a duplicate key as malformed", "[tweaks]") {
    // A JSON object may not repeat a key; a hand-edited dup is not parseable.
    CHECK_FALSE(
        read_edit_block("/*EDITMODE-BEGIN*/{\"a\":1,\"b\":2,\"a\":9}/*EDITMODE-END*/")
            .has_value());
}

TEST_CASE("set_edit_param updates a key and preserves surrounding bytes", "[tweaks]") {
    auto r = set_edit_param(kArtifact, "radius", "16");
    REQUIRE(r.has_value());
    CHECK(r->outside_bytes_intact);
    auto ps = read_edit_block(r->text);
    REQUIRE(ps.has_value());
    CHECK(param_value(*ps, "radius") == "16");
    CHECK(param_value(*ps, "accent") == "\"#33aaff\"");  // untouched
    // The prose around the block is byte-identical.
    CHECK(r->text.find("export const ui") == 0);
    CHECK(r->text.find("return render(p);") != std::string::npos);
}

TEST_CASE("set_edit_param appends a new key preserving existing order", "[tweaks]") {
    auto r = set_edit_param(kArtifact, "spacing", "4");
    REQUIRE(r.has_value());
    auto ps = read_edit_block(r->text);
    REQUIRE(ps.has_value());
    REQUIRE(ps->size() == 3);
    CHECK((*ps)[0].key == "accent");
    CHECK((*ps)[1].key == "radius");
    CHECK((*ps)[2].key == "spacing");  // appended last
    CHECK((*ps)[2].json_value == "4");
}

TEST_CASE("set_edit_param quotes a string value the caller supplies", "[tweaks]") {
    // Caller quotes strings; the block stays valid JSON.
    auto r = set_edit_param(kArtifact, "accent", "\"#ff0000\"");
    REQUIRE(r.has_value());
    auto ps = read_edit_block(r->text);
    CHECK(param_value(*ps, "accent") == "\"#ff0000\"");
}

TEST_CASE("rewrite is idempotent for an unchanged param set", "[tweaks]") {
    auto ps = read_edit_block(kArtifact);
    REQUIRE(ps.has_value());
    auto r = rewrite_edit_block(kArtifact, *ps);
    REQUIRE(r.has_value());
    CHECK(r->outside_bytes_intact);
    // Re-reading the rewritten text yields the same params.
    auto ps2 = read_edit_block(r->text);
    REQUIRE(ps2.has_value());
    REQUIRE(ps2->size() == ps->size());
    for (size_t i = 0; i < ps->size(); ++i) {
        CHECK((*ps2)[i].key == (*ps)[i].key);
        CHECK((*ps2)[i].json_value == (*ps)[i].json_value);
    }
    // A second rewrite is byte-stable.
    auto r2 = rewrite_edit_block(r->text, *ps2);
    REQUIRE(r2.has_value());
    CHECK(r2->text == r->text);
}

TEST_CASE("rewrite_edit_block on text with no block returns nullopt", "[tweaks]") {
    CHECK_FALSE(rewrite_edit_block("plain text", {{"a", "1"}}).has_value());
    CHECK_FALSE(set_edit_param("plain text", "a", "1").has_value());
}

TEST_CASE("edit_block_payload serializes empty params as an empty object", "[tweaks]") {
    CHECK(edit_block_payload({}) == "{}");
    CHECK(edit_block_payload({{"k", "true"}}) == "{\"k\":true}");
}

TEST_CASE("edit_block_payload escapes an awkward key", "[tweaks]") {
    auto payload = edit_block_payload({{"a\"b", "1"}});
    REQUIRE(payload.has_value());
    // The key is JSON-escaped so the payload round-trips.
    auto v = choc::json::parse(*payload);  // owning Value; bind before indexing
    CHECK(v.isObject());
    CHECK(v["a\"b"].getWithDefault<int>(0) == 1);
}

TEST_CASE("edit_block_payload normalizes a value, dropping trailing injection", "[tweaks]") {
    // A stored value that appends structure after a balanced value must not
    // survive: only the single JSON value is kept, the rest is discarded.
    auto payload = edit_block_payload({{"k", "1}/*EDITMODE-END*/ evil() /*x*/{\"y\":2"}});
    REQUIRE(payload.has_value());
    CHECK(*payload == "{\"k\":1}");  // the injected tail is gone
    // A value that is not exactly one JSON value is rejected.
    CHECK_FALSE(edit_block_payload({{"k", ""}}).has_value());
    CHECK_FALSE(edit_block_payload({{"k", "1,\"v\":2"}}).has_value());  // duplicate wrapper key
}

TEST_CASE("edit_block_payload rejects non-UTF-8 in a key", "[tweaks]") {
    std::string bad_key = "a";
    bad_key += static_cast<char>(0xF0);  // truncated multibyte lead byte
    CHECK_FALSE(edit_block_payload({{bad_key, "1"}}).has_value());
}

TEST_CASE("set_edit_param neutralizes a marker-injecting value", "[tweaks]") {
    std::string src = "before /*EDITMODE-BEGIN*/{\"a\":1}/*EDITMODE-END*/ after";
    // A malicious value tries to close the block early and inject code outside it.
    auto r = set_edit_param(src, "a", "1}/*EDITMODE-END*/ evil() /*x*/{\"a\":2");
    REQUIRE(r.has_value());
    CHECK(r->outside_bytes_intact);
    CHECK(r->text.find("evil()") == std::string::npos);  // injection did not survive
    // The block still round-trips to a single well-formed param.
    auto ps = read_edit_block(r->text);
    REQUIRE(ps.has_value());
    REQUIRE(ps->size() == 1);
    CHECK((*ps)[0].key == "a");
}

TEST_CASE("find_edit_block skips a marker inside a JS string to the real block", "[tweaks]") {
    // A literal marker in a string literal precedes the real block; the scan must
    // walk past the false match rather than give up.
    std::string src =
        "const doc = \"/*EDITMODE-BEGIN*/\";\n"
        "const p = /*EDITMODE-BEGIN*/{\"radius\":8}/*EDITMODE-END*/;\n";
    auto ps = read_edit_block(src);
    REQUIRE(ps.has_value());
    REQUIRE(ps->size() == 1);
    CHECK((*ps)[0].key == "radius");
    CHECK((*ps)[0].json_value == "8");
}

TEST_CASE("only the first block is targeted when two are present", "[tweaks]") {
    std::string two =
        "/*EDITMODE-BEGIN*/{\"a\":1}/*EDITMODE-END*/ and "
        "/*EDITMODE-BEGIN*/{\"b\":2}/*EDITMODE-END*/";
    auto ps = read_edit_block(two);
    REQUIRE(ps.has_value());
    REQUIRE(ps->size() == 1);
    CHECK((*ps)[0].key == "a");
    // A rewrite leaves the second block untouched.
    auto r = set_edit_param(two, "a", "5");
    REQUIRE(r.has_value());
    CHECK(r->text.find("{\"b\":2}") != std::string::npos);
    CHECK(r->outside_bytes_intact);
}
