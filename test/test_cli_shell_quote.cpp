// pulp #776 — pin the shell_quote contract.
//
// Until 2026-04-26, `shell_quote` Unix-style escaped backslashes
// unconditionally. On Windows that broke `git clone "C:\path"
// dest`: the URL written into `dest/.git/config` had doubled
// backslashes, so the next `git fetch origin` couldn't resolve the
// remote and `bump_one`'s origin/main redundancy gate silently
// fell through, returning "bumped" where the test expected "skipped".
//
// These tests assert the platform-correct shapes so the algorithm
// can't silently regress to the broken pre-#776 form. POSIX side
// keeps the original escape-`\`-and-`"` contract; Windows side
// follows the canonical MSVCRT argv-parsing rules from Microsoft's
// docs.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/cli_common.hpp"

#include <filesystem>

TEST_CASE("yaml_value reads plain and list-item scalars", "[cli][yaml][docs][issue-643]") {
    REQUIRE(yaml_value("slug: getting-started", "slug") == "getting-started");
    REQUIRE(yaml_value("  - slug: getting-started", "slug") == "getting-started");
    REQUIRE(yaml_value("      - name: ship", "name") == "ship");
    REQUIRE(yaml_value("      summary: Sign and package", "summary") == "Sign and package");
    REQUIRE(yaml_value("      - summary: list item", "summary") == "list item");
}

#ifdef _WIN32

TEST_CASE("shell_quote leaves Windows path backslashes literal", "[cli][shell-quote][issue-776]") {
    REQUIRE(shell_quote(std::string("C:\\Users\\foo")) == "\"C:\\Users\\foo\"");
}

TEST_CASE("shell_quote handles Windows paths with spaces", "[cli][shell-quote][issue-776]") {
    REQUIRE(shell_quote(std::string("C:\\Program Files\\git\\cmd")) ==
            "\"C:\\Program Files\\git\\cmd\"");
}

TEST_CASE("shell_quote escapes embedded quote per MSVCRT rules", "[cli][shell-quote][issue-776]") {
    // A literal " inside the argument needs one extra backslash so the
    // parser doesn't end the quoted region. Existing backslashes in the
    // run before the " are NOT doubled when there are none.
    //
    // Input:  say "hi"
    // Output: "say \"hi\""
    REQUIRE(shell_quote(std::string("say \"hi\"")) == "\"say \\\"hi\\\"\"");
}

TEST_CASE("shell_quote doubles backslashes that immediately precede an embedded quote",
          "[cli][shell-quote][issue-776]") {
    // For a run of N backslashes followed by ", the algorithm emits
    // (2N + 1) backslashes then the quote.
    //
    // Input:  foo\\"bar     (chars: f o o \ \ " b a r)
    // Output: "foo\\\\\\"bar"   (chars: " f o o \ \ \ \ \ " b a r ")
    REQUIRE(shell_quote(std::string("foo\\\\\"bar")) == "\"foo\\\\\\\\\\\"bar\"");
}

TEST_CASE("shell_quote doubles trailing backslashes so they don't escape the closing quote",
          "[cli][shell-quote][issue-776]") {
    // Input:  C:\foo\        (chars: C : \ f o o \)
    // Output: "C:\foo\\"     (the trailing run is doubled to 2 backslashes
    //                        before the closing ")
    REQUIRE(shell_quote(std::string("C:\\foo\\")) == "\"C:\\foo\\\\\"");
}

TEST_CASE("shell_quote leaves cmd.exe metacharacters inert inside quotes",
          "[cli][shell-quote][issue-776]") {
    // `&`, `|`, `<`, `>`, `%`, `!`, `^` are inert inside `"..."` to
    // cmd.exe — wrapping is enough; we don't need to ^-escape them.
    REQUIRE(shell_quote(std::string("a&b|c<d>e")) == "\"a&b|c<d>e\"");
}

#else  // POSIX

TEST_CASE("shell_quote escapes backslash and quote on POSIX", "[cli][shell-quote][issue-776]") {
    REQUIRE(shell_quote(std::string("/usr/bin/git")) == "\"/usr/bin/git\"");
    REQUIRE(shell_quote(std::string("/path with spaces/git")) ==
            "\"/path with spaces/git\"");
    REQUIRE(shell_quote(std::string("a\\b")) == "\"a\\\\b\"");
    // Input:  say "hi"
    // Output: "say \"hi\""
    REQUIRE(shell_quote(std::string("say \"hi\"")) == "\"say \\\"hi\\\"\"");
}

TEST_CASE("shell_quote wraps empty and POSIX metacharacter arguments", "[cli][shell-quote]") {
    REQUIRE(shell_quote(std::string()) == "\"\"");
    REQUIRE(shell_quote(std::string("a;b$(c)`d e")) == "\"a;b$(c)`d e\"");
}

TEST_CASE("shell_quote path overload matches string overload on POSIX", "[cli][shell-quote]") {
    const std::filesystem::path path = "/tmp/pulp path/with\"quote";
    REQUIRE(shell_quote(path) == shell_quote(path.string()));
}

#endif
