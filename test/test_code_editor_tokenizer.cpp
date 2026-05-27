// CodeEditor language-tokenizer tests
// (closes the gap-doc Phase 4 row "CodeEditor language coverage").
//
// Validates the per-language regex-lite tokenizers used by the
// CodeEditor's native paint path:
//   - C++ keywords + types + numbers + strings + comments + preprocessor,
//   - JavaScript keywords + types + template strings + comments,
//   - Python keywords + comments + triple-quoted strings,
//   - JSON literals + numbers + strings,
//   - Lua keywords + comments,
//   - Markdown headings + inline code + links,
//   - dispatch fallback for `PlainText` and other languages without
//     tokenizers returns a single `text` span.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <pulp/view/code_editor.hpp>
#include <pulp/view/code_editor_tokenizer.hpp>

using namespace pulp::view;

namespace {

bool has_token(const std::vector<Token>& tokens,
               std::string_view line,
               std::string_view span,
               TokenClass cls) {
    return std::any_of(tokens.begin(), tokens.end(), [&](const Token& t) {
        return t.cls == cls
            && t.start + t.length <= line.size()
            && line.substr(t.start, t.length) == span;
    });
}

const Token* find_at(const std::vector<Token>& tokens, std::size_t start) {
    for (const auto& t : tokens) {
        if (t.start == start) return &t;
    }
    return nullptr;
}

} // namespace

// ── Dispatch ─────────────────────────────────────────────────────────

TEST_CASE("tokenize_line: PlainText returns a single text span",
          "[code-editor][tokenizer]") {
    auto toks = tokenize_line("hello world", CodeLanguage::PlainText);
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0].cls == TokenClass::text);
    REQUIRE(toks[0].start == 0);
    REQUIRE(toks[0].length == 11);
}

TEST_CASE("tokenize_line: empty line yields no tokens",
          "[code-editor][tokenizer]") {
    auto cpp = tokenize_line("", CodeLanguage::Cpp);
    REQUIRE(cpp.empty());
    auto plain = tokenize_line("", CodeLanguage::PlainText);
    REQUIRE(plain.empty());
}

TEST_CASE("tokenize_line: unsupported languages fall back to text",
          "[code-editor][tokenizer]") {
    auto xml = tokenize_line("<tag>", CodeLanguage::XML);
    REQUIRE(xml.size() == 1);
    REQUIRE(xml[0].cls == TokenClass::text);

    auto faust = tokenize_line("process = _;", CodeLanguage::Faust);
    REQUIRE(faust.size() == 1);
    REQUIRE(faust[0].cls == TokenClass::text);

    auto jsfx = tokenize_line("@sample", CodeLanguage::JSFX);
    REQUIRE(jsfx.size() == 1);
    REQUIRE(jsfx[0].cls == TokenClass::text);

    auto glsl = tokenize_line("void main() {}", CodeLanguage::GLSL);
    REQUIRE(glsl.size() == 1);
    REQUIRE(glsl[0].cls == TokenClass::text);
}

// ── C++ ──────────────────────────────────────────────────────────────

TEST_CASE("tokenize_cpp_line: keywords + types + numbers + comments",
          "[code-editor][tokenizer][cpp]") {
    std::string_view line = "int main() { return 0; } // entry";
    auto toks = tokenize_cpp_line(line);

    REQUIRE(has_token(toks, line, "int", TokenClass::type));
    REQUIRE(has_token(toks, line, "return", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "0", TokenClass::number));
    REQUIRE(has_token(toks, line, "// entry", TokenClass::comment));
    REQUIRE_FALSE(has_token(toks, line, "main", TokenClass::keyword));
}

TEST_CASE("tokenize_cpp_line: string literals and escape sequences",
          "[code-editor][tokenizer][cpp]") {
    std::string_view line = R"(const char* s = "hello \"world\"";)";
    auto toks = tokenize_cpp_line(line);

    REQUIRE(has_token(toks, line, "const", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "char", TokenClass::type));
    REQUIRE(has_token(toks, line, R"("hello \"world\"")", TokenClass::string));
}

TEST_CASE("tokenize_cpp_line: preprocessor directives are a whole-line span",
          "[code-editor][tokenizer][cpp]") {
    std::string_view line = "#include <vector>";
    auto toks = tokenize_cpp_line(line);
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0].cls == TokenClass::preprocessor);
    REQUIRE(toks[0].start == 0);
    REQUIRE(toks[0].length == line.size());
}

TEST_CASE("tokenize_cpp_line: block comment and hex numbers",
          "[code-editor][tokenizer][cpp]") {
    std::string_view line = "auto x = 0xFF; /* hex */";
    auto toks = tokenize_cpp_line(line);
    REQUIRE(has_token(toks, line, "auto", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "0xFF", TokenClass::number));
    REQUIRE(has_token(toks, line, "/* hex */", TokenClass::comment));
}

TEST_CASE("tokenize_cpp_line: unterminated block comment paints to end",
          "[code-editor][tokenizer][cpp]") {
    std::string_view line = "/* open";
    auto toks = tokenize_cpp_line(line);
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0].cls == TokenClass::comment);
    REQUIRE(toks[0].length == line.size());
}

// ── JavaScript ───────────────────────────────────────────────────────

TEST_CASE("tokenize_javascript_line: keywords + template strings",
          "[code-editor][tokenizer][js]") {
    std::string_view line = "const x = `hello ${name}`;";
    auto toks = tokenize_javascript_line(line);
    REQUIRE(has_token(toks, line, "const", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "`hello ${name}`", TokenClass::string));
}

TEST_CASE("tokenize_javascript_line: types and arrow function",
          "[code-editor][tokenizer][js]") {
    std::string_view line = "let p = new Promise((r) => r());";
    auto toks = tokenize_javascript_line(line);
    REQUIRE(has_token(toks, line, "let", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "new", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "Promise", TokenClass::type));
}

// ── Python ───────────────────────────────────────────────────────────

TEST_CASE("tokenize_python_line: def + keywords + hash comment",
          "[code-editor][tokenizer][python]") {
    std::string_view line = "def fn(x): return x + 1  # add one";
    auto toks = tokenize_python_line(line);
    REQUIRE(has_token(toks, line, "def", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "return", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "1", TokenClass::number));
    REQUIRE(has_token(toks, line, "# add one", TokenClass::comment));
}

TEST_CASE("tokenize_python_line: triple-quoted string on one line",
          "[code-editor][tokenizer][python]") {
    std::string_view line = R"(s = """doc""")";
    auto toks = tokenize_python_line(line);
    REQUIRE(has_token(toks, line, R"("""doc""")", TokenClass::string));
}

TEST_CASE("tokenize_python_line: unterminated triple-quoted string spans to EOL",
          "[code-editor][tokenizer][python]") {
    std::string_view line = R"(s = """open)";
    auto toks = tokenize_python_line(line);
    auto* t = find_at(toks, 4);
    REQUIRE(t != nullptr);
    REQUIRE(t->cls == TokenClass::string);
    REQUIRE(t->length == line.size() - 4);
}

// ── JSON ─────────────────────────────────────────────────────────────

TEST_CASE("tokenize_json_line: string keys + literals + numbers",
          "[code-editor][tokenizer][json]") {
    std::string_view line = R"({ "ok": true, "n": -3.14, "v": null })";
    auto toks = tokenize_json_line(line);
    REQUIRE(has_token(toks, line, "\"ok\"", TokenClass::string));
    REQUIRE(has_token(toks, line, "true", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "-3.14", TokenClass::number));
    REQUIRE(has_token(toks, line, "null", TokenClass::keyword));
}

// ── Lua ──────────────────────────────────────────────────────────────

TEST_CASE("tokenize_lua_line: function + double-dash comment",
          "[code-editor][tokenizer][lua]") {
    std::string_view line = "local function f() return 42 end -- done";
    auto toks = tokenize_lua_line(line);
    REQUIRE(has_token(toks, line, "local", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "function", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "return", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "end", TokenClass::keyword));
    REQUIRE(has_token(toks, line, "42", TokenClass::number));
    REQUIRE(has_token(toks, line, "-- done", TokenClass::comment));
}

// ── Markdown ─────────────────────────────────────────────────────────

TEST_CASE("tokenize_markdown_line: heading line emits one heading span",
          "[code-editor][tokenizer][markdown]") {
    std::string_view line = "## Section title";
    auto toks = tokenize_markdown_line(line);
    REQUIRE(toks.size() == 1);
    REQUIRE(toks[0].cls == TokenClass::heading);
    REQUIRE(toks[0].length == line.size());
}

TEST_CASE("tokenize_markdown_line: inline code and link",
          "[code-editor][tokenizer][markdown]") {
    std::string_view line = "see `Widget` in [docs](http://x)";
    auto toks = tokenize_markdown_line(line);
    REQUIRE(has_token(toks, line, "`Widget`", TokenClass::string));
    REQUIRE(has_token(toks, line, "[docs](http://x)", TokenClass::link));
}

TEST_CASE("tokenize_markdown_line: too many leading hashes is not a heading",
          "[code-editor][tokenizer][markdown]") {
    std::string_view line = "####### not a heading";
    auto toks = tokenize_markdown_line(line);
    // 7 hashes — Markdown caps at 6, so this should NOT be a heading.
    bool any_heading = std::any_of(toks.begin(), toks.end(),
        [](const Token& t) { return t.cls == TokenClass::heading; });
    REQUIRE_FALSE(any_heading);
}

// ── is_keyword ───────────────────────────────────────────────────────

TEST_CASE("is_keyword classifies per-language sets correctly",
          "[code-editor][tokenizer][keyword-set]") {
    REQUIRE(is_keyword("return", CodeLanguage::Cpp));
    REQUIRE(is_keyword("class",  CodeLanguage::Cpp));
    REQUIRE_FALSE(is_keyword("Promise", CodeLanguage::Cpp));

    REQUIRE(is_keyword("function", CodeLanguage::JavaScript));
    REQUIRE_FALSE(is_keyword("function", CodeLanguage::Cpp));

    REQUIRE(is_keyword("def",   CodeLanguage::Python));
    REQUIRE(is_keyword("true",  CodeLanguage::JSON));
    REQUIRE(is_keyword("end",   CodeLanguage::Lua));
    REQUIRE_FALSE(is_keyword("notakeyword", CodeLanguage::Lua));
    REQUIRE_FALSE(is_keyword("if", CodeLanguage::PlainText));
}
