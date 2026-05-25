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
#include <limits>

TEST_CASE("yaml_value reads plain and list-item scalars", "[cli][yaml][docs][issue-643]") {
    REQUIRE(yaml_value("slug: getting-started", "slug") == "getting-started");
    REQUIRE(yaml_value("  - slug: getting-started", "slug") == "getting-started");
    REQUIRE(yaml_value("      - name: ship", "name") == "ship");
    REQUIRE(yaml_value("      summary: Sign and package", "summary") == "Sign and package");
    REQUIRE(yaml_value("      - summary: list item", "summary") == "list item");
}

TEST_CASE("yaml_value rejects mismatched keys and empty values", "[cli][yaml][coverage]") {
    REQUIRE(yaml_value("slug_name: wrong", "slug").empty());
    REQUIRE(yaml_value("slug", "slug").empty());
    REQUIRE(yaml_value("slug:", "slug").empty());
    REQUIRE(yaml_value("  - slug:", "slug").empty());
    REQUIRE(yaml_value("name: ship", "slug").empty());
    REQUIRE(yaml_value("  - name: ship", "slug").empty());
}

TEST_CASE("cli string helpers preserve command-facing contracts", "[cli][common][coverage]") {
    REQUIRE(trim("  \tPulp\n") == "Pulp");
    REQUIRE(strip_quotes("\"quoted value\"") == "quoted value");
    REQUIRE(strip_quotes("'quoted value'") == "quoted value");
    REQUIRE(strip_quotes("\"unterminated") == "\"unterminated");
    REQUIRE(replace_all_str("one two one", "one", "three") == "three two three");
    REQUIRE(icontains("Audio Plugin Framework", "plugin"));
    REQUIRE(icontains("Audio Plugin Framework", "PLUGIN"));
    REQUIRE_FALSE(icontains("Audio Plugin Framework", "video"));
    REQUIRE(icontains("Audio Plugin Framework", ""));
    REQUIRE(sanitize_process_output(std::string("ok\0hidden\n", 10)) == "okhidden\n");
    REQUIRE(truncate_message("short", 10) == "short");
    REQUIRE(truncate_message("abcdef", 3) == "abc...");
}

TEST_CASE("parse_size_arg accepts decimal integers without disturbing failures",
          "[cli][common][coverage]") {
    std::size_t value = 77;
    REQUIRE(parse_size_arg("0", "--top", value));
    REQUIRE(value == 0);
    REQUIRE(parse_size_arg("42", "--top", value));
    REQUIRE(value == 42);
    REQUIRE(parse_size_arg(std::to_string(std::numeric_limits<std::size_t>::max()),
                           "--top",
                           value));
    REQUIRE(value == std::numeric_limits<std::size_t>::max());

    value = 1234;
    REQUIRE_FALSE(parse_size_arg("", "--top", value));
    REQUIRE(value == 1234);
    REQUIRE_FALSE(parse_size_arg("-1", "--top", value));
    REQUIRE(value == 1234);
    REQUIRE(parse_size_arg("+1", "--top", value));
    REQUIRE(value == 1);
    value = 1234;
    REQUIRE_FALSE(parse_size_arg("10x", "--top", value));
    REQUIRE(value == 1234);
    REQUIRE_FALSE(parse_size_arg("1.5", "--top", value));
    REQUIRE(value == 1234);
}

TEST_CASE("parse_double_arg accepts finite decimals and rejects non-finite text",
          "[cli][common][coverage]") {
    double value = 9.0;
    REQUIRE(parse_double_arg("0", "--min-score", value));
    REQUIRE(value == 0.0);
    REQUIRE(parse_double_arg("0.75", "--min-score", value));
    REQUIRE(value == 0.75);
    REQUIRE(parse_double_arg("-2.5", "--min-score", value));
    REQUIRE(value == -2.5);

    value = 4.25;
    REQUIRE_FALSE(parse_double_arg("", "--min-score", value));
    REQUIRE(value == 4.25);
    REQUIRE_FALSE(parse_double_arg("nan", "--min-score", value));
    REQUIRE(value == 4.25);
    REQUIRE_FALSE(parse_double_arg("inf", "--min-score", value));
    REQUIRE(value == 4.25);
    REQUIRE_FALSE(parse_double_arg("0.5x", "--min-score", value));
    REQUIRE(value == 4.25);
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
