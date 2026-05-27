// Regression for URL parser + percent-encoding + query helpers
// (gap-doc Runtime row "URL"). Pure parser — no network IO.

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/url.hpp>

using pulp::runtime::Url;
using pulp::runtime::percent_encode;
using pulp::runtime::percent_decode;
using pulp::runtime::form_encode;
using pulp::runtime::form_decode;
using pulp::runtime::parse_query;
using pulp::runtime::build_query;

TEST_CASE("Url parses scheme://host:port/path?query#fragment", "[runtime][url]") {
    auto u = Url::parse("https://example.com:8443/v1/items?id=42&q=hello#top");
    REQUIRE(u.has_value());
    REQUIRE(u->scheme == "https");
    REQUIRE(u->host == "example.com");
    REQUIRE(u->port == 8443);
    REQUIRE(u->path == "/v1/items");
    REQUIRE(u->query == "id=42&q=hello");
    REQUIRE(u->fragment == "top");
    REQUIRE(u->effective_port() == 8443);
}

TEST_CASE("Url parses without authority", "[runtime][url]") {
    auto u = Url::parse("mailto:nobody@example.com");
    REQUIRE(u.has_value());
    REQUIRE(u->scheme == "mailto");
    REQUIRE(u->path == "nobody@example.com");
    REQUIRE(u->host.empty());
}

TEST_CASE("Url parses userinfo", "[runtime][url]") {
    auto u = Url::parse("ftp://anon:passwd@ftp.example.com:2121/pub");
    REQUIRE(u.has_value());
    REQUIRE(u->user == "anon");
    REQUIRE(u->password == "passwd");
    REQUIRE(u->host == "ftp.example.com");
    REQUIRE(u->port == 2121);
    REQUIRE(u->path == "/pub");
}

TEST_CASE("Url parses bracketed IPv6 host", "[runtime][url]") {
    auto u = Url::parse("http://[2001:db8::1]:8080/");
    REQUIRE(u.has_value());
    REQUIRE(u->host == "2001:db8::1");
    REQUIRE(u->port == 8080);

    auto bad = Url::parse("http://[2001:db8::1/path");  // unterminated
    REQUIRE_FALSE(bad.has_value());
}

TEST_CASE("Url default_port and effective_port", "[runtime][url]") {
    auto u = Url::parse("https://example.com/x");
    REQUIRE(u.has_value());
    REQUIRE(u->port == 0);
    REQUIRE(u->default_port() == 443);
    REQUIRE(u->effective_port() == 443);

    auto u2 = Url::parse("ws://example.com:9000/socket");
    REQUIRE(u2->effective_port() == 9000);

    auto u3 = Url::parse("custom://host/x");
    REQUIRE(u3->default_port() == 0);
    REQUIRE(u3->effective_port() == 0);
}

TEST_CASE("Url rejects malformed input", "[runtime][url]") {
    REQUIRE_FALSE(Url::parse("").has_value());
    REQUIRE_FALSE(Url::parse("noscheme").has_value());
    REQUIRE_FALSE(Url::parse(":nohost").has_value());
    REQUIRE_FALSE(Url::parse("9scheme://x").has_value());     // scheme must start ALPHA
    REQUIRE_FALSE(Url::parse("http://host:99999/").has_value()); // port > 65535
    REQUIRE_FALSE(Url::parse("http://host:abc/").has_value());   // non-numeric port
    REQUIRE_FALSE(Url::is_valid("not a url"));
}

// Regression for the Codex P2 review comment "Reject non-numeric URL
// ports" — strtol() alone accepted "80abc" as a valid port-80 URL.
// After the fix the entire port_text must be all decimal digits.
TEST_CASE("Url rejects port with trailing junk", "[runtime][url][codex-p2]") {
    REQUIRE_FALSE(Url::parse("http://example.com:80abc/path").has_value());
    REQUIRE_FALSE(Url::parse("http://example.com:8080x/").has_value());
    // Same fix on the IPv6 branch.
    REQUIRE_FALSE(Url::parse("http://[::1]:80junk/").has_value());
    // Leading + / leading 0s with junk also rejected.
    REQUIRE_FALSE(Url::parse("http://example.com:+80/").has_value());
    // Pure-digit ports still parse cleanly.
    auto ok = Url::parse("http://example.com:8080/");
    REQUIRE(ok.has_value());
    REQUIRE(ok->port == 8080);
}

TEST_CASE("Url::to_string round-trips", "[runtime][url]") {
    auto u = Url::parse("https://user:pw@api.example.com:8443/v1?x=1#frag");
    REQUIRE(u.has_value());
    auto s = u->to_string();
    auto u2 = Url::parse(s);
    REQUIRE(u2.has_value());
    REQUIRE(u2->scheme == u->scheme);
    REQUIRE(u2->user == u->user);
    REQUIRE(u2->password == u->password);
    REQUIRE(u2->host == u->host);
    REQUIRE(u2->port == u->port);
    REQUIRE(u2->path == u->path);
    REQUIRE(u2->query == u->query);
    REQUIRE(u2->fragment == u->fragment);
}

TEST_CASE("percent_encode / percent_decode round-trip", "[runtime][url]") {
    auto s = std::string("hello world / foo? bar=baz&qux");
    auto enc = percent_encode(s);
    REQUIRE(enc.find(' ') == std::string::npos);
    REQUIRE(enc.find('/') == std::string::npos);
    REQUIRE(percent_decode(enc) == s);
    // Unreserved characters survive untouched.
    REQUIRE(percent_encode("ABCabc-_.~") == "ABCabc-_.~");
    // Invalid escapes pass through unchanged.
    REQUIRE(percent_decode("%ZZ") == "%ZZ");
}

TEST_CASE("form_encode uses '+' for spaces", "[runtime][url]") {
    REQUIRE(form_encode("hello world") == "hello+world");
    REQUIRE(form_decode("hello+world") == "hello world");
    REQUIRE(form_decode("a%20b") == "a b");
}

TEST_CASE("parse_query handles separators and missing values",
          "[runtime][url]") {
    auto p = parse_query("?a=1&b=2;c=&d");
    REQUIRE(p.size() == 4);
    REQUIRE(p[0] == std::make_pair(std::string("a"), std::string("1")));
    REQUIRE(p[1] == std::make_pair(std::string("b"), std::string("2")));
    REQUIRE(p[2] == std::make_pair(std::string("c"), std::string("")));
    REQUIRE(p[3] == std::make_pair(std::string("d"), std::string("")));

    auto empty = parse_query("");
    REQUIRE(empty.empty());
}

TEST_CASE("build_query encodes pairs", "[runtime][url]") {
    auto q = build_query({{"name", "alice"}, {"q", "hello world"}});
    REQUIRE(q == "name=alice&q=hello+world");
    REQUIRE(build_query({}) == "");
}
