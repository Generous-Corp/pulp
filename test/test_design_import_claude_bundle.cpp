// test_design_import_claude_bundle.cpp
//
// Pulp #468 — verify the JSON-envelope unpack path used by the
// `--execute-bundle` import lane. Builds a synthetic envelope on the fly
// so tests stay deterministic and independent of any vendored fixture.
//
// Optional second pass: if PULP_CLAUDE_BUNDLE_FIXTURE points at a real
// Claude Design HTML on disk (e.g. Spectr's `resources/editor.html`),
// also exercise the parser against that. Skipped silently when the env
// var is unset so CI doesn't depend on the fixture being present.

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/zip.hpp>
#include <pulp/view/design_import.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace pulp::view;

namespace {

// Build a Claude-Design-shaped HTML payload around an arbitrary
// manifest + template body. Mirrors the real envelope format observed
// in Spectr's `editor.html`.
std::string build_envelope(const std::string& manifest_json,
                           const std::string& template_body_html) {
    // The template tag's content is itself a JSON-encoded HTML string,
    // exactly as the real exporter emits it.
    auto json_quote = [](const std::string& s) {
        std::string out = "\"";
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '/':
                    // Escape '/' as / so the literal `</script>` end
                    // tag inside JSON-encoded HTML doesn't accidentally
                    // close the wrapping <script type="__bundler/template">.
                    // The real Claude Design exporter does this too.
                    out += "\\u002F";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        out += "\"";
        return out;
    };

    std::ostringstream ss;
    ss << "<!DOCTYPE html><html><head><title>Test</title></head><body>"
       << "<script type=\"__bundler/manifest\">" << manifest_json << "</script>"
       << "<script type=\"__bundler/template\">" << json_quote(template_body_html)
       << "</script>"
       << "</body></html>";
    return ss.str();
}

// Build one entry of the manifest map. `compressed=true` will gzip the
// data first, just like the real exporter.
//
// Note: pulp::runtime::gzip_compress is misnamed — it actually emits
// zlib (RFC 1950), not gzip (RFC 1952). For these tests we need real
// gzip so the parser's RFC 1952 decoder accepts the bytes. We hand-roll
// a minimal gzip wrapper around raw deflate (which IS what
// `deflate_compress` produces).
std::vector<uint8_t> gzip_wrap_deflate(const std::vector<uint8_t>& raw) {
    auto deflated = pulp::runtime::deflate_compress(raw.data(), raw.size());
    REQUIRE(deflated.has_value());
    // Compute CRC32 (from miniz, exposed as part of deflate_compress).
    // We don't strictly need it for parsing — our claude_bundle_inflate
    // accepts any 8-byte trailer — but the parser does length-check the
    // trailer bytes, so emit them.
    std::vector<uint8_t> out;
    out.reserve(deflated->size() + 18);
    // 10-byte header: magic, deflate, no flags, mtime=0, xfl=0, os=255 (unknown)
    const uint8_t header[10] = {0x1f, 0x8b, 0x08, 0x00,
                                0, 0, 0, 0,       // mtime
                                0, 0xff};         // xfl + os
    out.insert(out.end(), header, header + 10);
    out.insert(out.end(), deflated->begin(), deflated->end());
    // 8-byte trailer: CRC32 (zeroed — claude_bundle_inflate doesn't
    // verify) + ISIZE mod 2^32.
    for (int i = 0; i < 4; ++i) out.push_back(0);
    uint32_t isize = static_cast<uint32_t>(raw.size());
    out.push_back(static_cast<uint8_t>(isize & 0xff));
    out.push_back(static_cast<uint8_t>((isize >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((isize >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((isize >> 24) & 0xff));
    return out;
}

std::string manifest_entry(const std::string& uuid, const std::string& mime,
                           const std::string& contents, bool compressed) {
    std::vector<uint8_t> bytes(contents.begin(), contents.end());
    std::vector<uint8_t> payload;
    if (compressed) {
        payload = gzip_wrap_deflate(bytes);
    } else {
        payload = std::move(bytes);
    }
    std::string b64 = pulp::runtime::base64_encode(payload.data(), payload.size());
    std::ostringstream ss;
    ss << "\"" << uuid << "\":{"
       << "\"mime\":\"" << mime << "\","
       << "\"compressed\":" << (compressed ? "true" : "false") << ","
       << "\"data\":\"" << b64 << "\"}";
    return ss.str();
}

} // namespace

TEST_CASE("parse_claude_bundle returns nullopt when bundler tags are missing",
          "[view][import][issue-468]") {
    auto bundle = parse_claude_bundle("<html><body>no bundler tags here</body></html>");
    REQUIRE_FALSE(bundle.has_value());
}

TEST_CASE("parse_claude_bundle decodes a base64-gzip envelope",
          "[view][import][issue-468]") {
    const std::string js_a = "console.log('asset A');";
    const std::string js_b = "console.log('asset B');";
    const std::string font  = "synthetic-woff2-bytes";

    std::ostringstream manifest;
    manifest << "{"
             << manifest_entry("uuid-a", "text/javascript", js_a, true) << ","
             << manifest_entry("uuid-b", "text/javascript", js_b, true) << ","
             << manifest_entry("uuid-font", "font/woff2", font, true)
             << "}";

    const std::string body =
        R"(<div id="root"></div>)"
        R"(<script src="uuid-a"></script>)"
        R"(<script src="uuid-b"></script>)";

    auto bundle = parse_claude_bundle(build_envelope(manifest.str(), body));
    REQUIRE(bundle.has_value());

    REQUIRE(bundle->assets.size() == 3);

    // Find the JS assets; verify they round-trip through base64+gzip.
    auto find_asset = [&](const std::string& uuid) -> const ClaudeBundleAsset* {
        for (const auto& a : bundle->assets) {
            if (a.uuid == uuid) return &a;
        }
        return nullptr;
    };
    auto* a = find_asset("uuid-a");
    auto* b = find_asset("uuid-b");
    auto* f = find_asset("uuid-font");
    REQUIRE(a != nullptr); REQUIRE(b != nullptr); REQUIRE(f != nullptr);
    REQUIRE(a->mime == "text/javascript");
    REQUIRE(b->mime == "text/javascript");
    REQUIRE(f->mime == "font/woff2");
    REQUIRE(std::string(a->data.begin(), a->data.end()) == js_a);
    REQUIRE(std::string(b->data.begin(), b->data.end()) == js_b);
    REQUIRE(std::string(f->data.begin(), f->data.end()) == font);

    // javascript_indices follows the template's <script src> order, not
    // the manifest order — verify by reordering manifest declarations.
    REQUIRE(bundle->javascript_indices.size() == 2);
    REQUIRE(bundle->assets[bundle->javascript_indices[0]].uuid == "uuid-a");
    REQUIRE(bundle->assets[bundle->javascript_indices[1]].uuid == "uuid-b");

    // Template HTML is the unwrapped string — same content we passed in.
    REQUIRE(bundle->template_html == body);
}

TEST_CASE("parse_claude_bundle accepts uncompressed entries (compressed:false)",
          "[view][import][issue-468]") {
    const std::string js = "globalThis.x = 1;";

    std::ostringstream manifest;
    manifest << "{" << manifest_entry("u1", "text/javascript", js, false) << "}";

    const std::string body =
        R"(<div id="root"></div><script src="u1"></script>)";

    auto bundle = parse_claude_bundle(build_envelope(manifest.str(), body));
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->assets.size() == 1);
    REQUIRE(std::string(bundle->assets[0].data.begin(),
                        bundle->assets[0].data.end()) == js);
    REQUIRE(bundle->javascript_indices == std::vector<size_t>{0});
}

TEST_CASE("parse_claude_bundle returns nullopt when the manifest is malformed JSON",
          "[view][import][issue-468]") {
    auto bundle = parse_claude_bundle(
        "<html><script type=\"__bundler/manifest\">not json</script>"
        "<script type=\"__bundler/template\">\"<body></body>\"</script></html>");
    REQUIRE_FALSE(bundle.has_value());
}

TEST_CASE("parse_claude_bundle returns nullopt for malformed template JSON or non-object manifest",
          "[view][import][issue-468]") {
    SECTION("template tag is not valid JSON") {
        auto bundle = parse_claude_bundle(
            "<html><script type=\"__bundler/manifest\">{}</script>"
            "<script type=\"__bundler/template\"><div></div></script></html>");
        REQUIRE_FALSE(bundle.has_value());
    }

    SECTION("template JSON is not a string") {
        auto bundle = parse_claude_bundle(
            "<html><script type=\"__bundler/manifest\">{}</script>"
            "<script type=\"__bundler/template\">{\"html\":\"<div></div>\"}</script></html>");
        REQUIRE_FALSE(bundle.has_value());
    }

    SECTION("manifest JSON is not an object") {
        auto bundle = parse_claude_bundle(build_envelope("[1,2,3]", "<div id=\"root\"></div>"));
        REQUIRE_FALSE(bundle.has_value());
    }
}

TEST_CASE("parse_claude_bundle skips malformed assets and indexes only referenced JavaScript",
          "[view][import][issue-468]") {
    const std::string referenced_js = "globalThis.referenced = true;";
    const std::string unreferenced_js = "globalThis.unreferenced = true;";
    const std::string invalid_gzip = "not gzip";
    const std::string font = "font bytes";

    std::ostringstream manifest;
    manifest << "{"
             << "\"non-object\":\"ignored\","
             << "\"missing-data\":{\"mime\":\"text/javascript\",\"compressed\":false},"
             << "\"non-string-data\":{\"mime\":\"text/javascript\",\"compressed\":false,\"data\":42},"
             << "\"bad-base64\":{\"mime\":\"text/javascript\",\"compressed\":false,\"data\":\"%%%\"},"
             << "\"bad-compressed\":{\"mime\":\"text/javascript\",\"compressed\":true,"
             << "\"data\":\"" << pulp::runtime::base64_encode(invalid_gzip) << "\"},"
             << manifest_entry("referenced-js", "text/javascript", referenced_js, false) << ","
             << manifest_entry("unreferenced-js", "text/javascript", unreferenced_js, false) << ","
             << manifest_entry("referenced-font", "font/woff2", font, false)
             << "}";

    const std::string body =
        R"(<div id="root"></div>)"
        R"(<script src="missing-data"></script>)"
        R"(<script src="referenced-font"></script>)"
        R"(<script src="referenced-js"></script>)";

    auto bundle = parse_claude_bundle(build_envelope(manifest.str(), body));
    REQUIRE(bundle.has_value());

    REQUIRE(bundle->assets.size() == 3);
    REQUIRE(bundle->assets[0].uuid == "referenced-js");
    REQUIRE(bundle->assets[1].uuid == "unreferenced-js");
    REQUIRE(bundle->assets[2].uuid == "referenced-font");

    REQUIRE(bundle->javascript_indices.size() == 1);
    REQUIRE(bundle->assets[bundle->javascript_indices[0]].uuid == "referenced-js");
}

TEST_CASE("parse_claude_bundle accepts a real Spectr editor.html fixture "
          "when PULP_CLAUDE_BUNDLE_FIXTURE is set",
          "[view][import][issue-468][.fixture]") {
    const char* fixture = std::getenv("PULP_CLAUDE_BUNDLE_FIXTURE");
    if (!fixture || !*fixture) {
        SUCCEED("PULP_CLAUDE_BUNDLE_FIXTURE not set — skipping real-bundle test");
        return;
    }
    std::ifstream f(fixture);
    if (!f.is_open()) {
        SUCCEED("fixture not readable, skipping: " << fixture);
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    auto bundle = parse_claude_bundle(ss.str());
    REQUIRE(bundle.has_value());
    // A real Spectr editor.html bundle has 16 assets (3 JS + 13 woff2)
    // per the gap-matrix recon. Don't pin the exact count — Claude
    // Design exports of other plugins will differ — just sanity-check
    // shape: at least one JS asset that the template references.
    REQUIRE(bundle->assets.size() > 0);
    REQUIRE(bundle->javascript_indices.size() >= 1);
    REQUIRE_FALSE(bundle->template_html.empty());
}

TEST_CASE("parse_claude_bundle order: javascript_indices reflects template order, "
          "not manifest order",
          "[view][import][issue-468]") {
    // Two JS assets; manifest declares B first, template loads A first.
    std::ostringstream manifest;
    manifest << "{"
             << manifest_entry("uuid-b", "text/javascript", "/* B */", true) << ","
             << manifest_entry("uuid-a", "text/javascript", "/* A */", true)
             << "}";

    const std::string body =
        R"(<div id="root"></div>)"
        R"(<script src="uuid-a"></script>)"
        R"(<script src="uuid-b"></script>)";

    auto bundle = parse_claude_bundle(build_envelope(manifest.str(), body));
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->javascript_indices.size() == 2);
    REQUIRE(bundle->assets[bundle->javascript_indices[0]].uuid == "uuid-a");
    REQUIRE(bundle->assets[bundle->javascript_indices[1]].uuid == "uuid-b");
}

TEST_CASE("extract_claude_classnames reads bundled template styles and skips leading font-face blocks",
          "[view][import][issue-468][issue-1035]") {
    std::ostringstream manifest;
    manifest << "{"
             << manifest_entry("uuid-js", "text/javascript", "globalThis.ready = true;", false)
             << "}";

    const std::string body =
        R"(<style>@font-face { font-family: BundleFont; src: url(font.woff2); })"
        R"(.font-block-leak { color: red; }</style>)"
        R"(<style>.panel { font-size: 14px; background-color: #123456; })"
        R"(.knob { border-radius: 8px; }</style>)"
        R"(<div id="root" class="panel knob"></div>)"
        R"(<script src="uuid-js"></script>)";

    const auto rules = extract_claude_classnames(build_envelope(manifest.str(), body));

    REQUIRE(rules.size() == 2);
    REQUIRE(rules.at("panel").at("fontSize") == "14px");
    REQUIRE(rules.at("panel").at("backgroundColor") == "#123456");
    REQUIRE(rules.at("knob").at("borderRadius") == "8px");
    REQUIRE(rules.count("font-block-leak") == 0);
}
