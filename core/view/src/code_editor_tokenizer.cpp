#include <pulp/view/code_editor_tokenizer.hpp>

#include <array>
#include <cctype>
#include <string_view>

// Lightweight, allocation-friendly tokenizers used by the
// CodeEditor's native paint path. Each language tokenizer walks the
// line character-by-character and emits non-overlapping `Token` spans.
// Multi-line constructs (block comments, multi-line strings) emit the
// opener on the first line only — the editor paints each line
// independently and re-tokenizes on every paint, so spanning state
// across lines is not worth the complexity in the fallback renderer.

namespace pulp::view {

namespace {

constexpr bool is_id_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
constexpr bool is_id_cont(char c) {
    return is_id_start(c) || (c >= '0' && c <= '9');
}
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_hex_digit(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Linear keyword lookup. The lists are small (<60 entries) so a sorted
// std::lower_bound is not measurably faster than the obvious scan.
bool contains(std::initializer_list<std::string_view> set,
              std::string_view word) {
    for (auto s : set) {
        if (s == word) return true;
    }
    return false;
}

constexpr std::array<std::string_view, 60> kCppKeywords{
    "alignas", "alignof", "and", "asm", "auto", "break", "case",
    "catch", "class", "concept", "const", "consteval", "constexpr",
    "constinit", "continue", "co_await", "co_return", "co_yield",
    "decltype", "default", "delete", "do", "else", "enum", "explicit",
    "export", "extern", "false", "final", "for", "friend", "goto",
    "if", "inline", "module", "mutable", "namespace", "new", "noexcept",
    "not", "nullptr", "operator", "or", "private", "protected", "public",
    "requires", "return", "sizeof", "static", "static_assert", "struct",
    "switch", "template", "this", "throw", "true", "try", "typedef",
    "typename"};

constexpr std::array<std::string_view, 18> kCppTypes{
    "void", "bool", "char", "char8_t", "char16_t", "char32_t",
    "wchar_t", "short", "int", "long", "signed", "unsigned", "float",
    "double", "size_t", "ptrdiff_t", "int64_t", "uint64_t"};

constexpr std::array<std::string_view, 38> kJsKeywords{
    "async", "await", "break", "case", "catch", "class", "const",
    "continue", "debugger", "default", "delete", "do", "else", "enum",
    "export", "extends", "false", "finally", "for", "function",
    "if", "import", "in", "instanceof", "let", "new", "null", "of",
    "return", "static", "super", "switch", "this", "throw", "true",
    "try", "typeof", "var"};

constexpr std::array<std::string_view, 8> kJsTypes{
    "Number", "String", "Boolean", "Object", "Array", "Symbol",
    "Promise", "BigInt"};

constexpr std::array<std::string_view, 35> kPythonKeywords{
    "and", "as", "assert", "async", "await", "break", "class",
    "continue", "def", "del", "elif", "else", "except", "False",
    "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "None", "nonlocal", "not", "or", "pass", "raise",
    "return", "True", "try", "while", "with", "yield"};

constexpr std::array<std::string_view, 22> kLuaKeywords{
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while"};

constexpr std::array<std::string_view, 3> kJsonKeywords{
    "true", "false", "null"};

template <std::size_t N>
bool in_set(const std::array<std::string_view, N>& set,
            std::string_view word) {
    for (const auto& s : set) {
        if (s == word) return true;
    }
    return false;
}

// Scan a string literal beginning at `start` whose quote character is
// `line[start]`. Returns the index *after* the closing quote, or
// `line.size()` if the string runs to the end of the line.
std::size_t scan_string(std::string_view line, std::size_t start) {
    char quote = line[start];
    std::size_t i = start + 1;
    while (i < line.size()) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            i += 2;
            continue;
        }
        if (line[i] == quote) {
            return i + 1;
        }
        ++i;
    }
    return line.size();
}

// Scan a numeric literal beginning at `start`. Handles 0x / 0b prefixes
// + decimal + simple float exponent. Returns the index past the literal.
std::size_t scan_number(std::string_view line, std::size_t start) {
    std::size_t i = start;
    if (line[i] == '0' && i + 1 < line.size()
        && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
        i += 2;
        while (i < line.size() && (is_hex_digit(line[i]) || line[i] == '_')) ++i;
        return i;
    }
    if (line[i] == '0' && i + 1 < line.size()
        && (line[i + 1] == 'b' || line[i + 1] == 'B')) {
        i += 2;
        while (i < line.size() && (line[i] == '0' || line[i] == '1' || line[i] == '_')) ++i;
        return i;
    }
    while (i < line.size() && (is_digit(line[i]) || line[i] == '_')) ++i;
    if (i < line.size() && line[i] == '.') {
        ++i;
        while (i < line.size() && (is_digit(line[i]) || line[i] == '_')) ++i;
    }
    if (i < line.size() && (line[i] == 'e' || line[i] == 'E')) {
        ++i;
        if (i < line.size() && (line[i] == '+' || line[i] == '-')) ++i;
        while (i < line.size() && is_digit(line[i])) ++i;
    }
    // Type suffix (f, L, u, ll, etc.) — keep it inside the number span.
    while (i < line.size()
           && (line[i] == 'f' || line[i] == 'F' || line[i] == 'l'
               || line[i] == 'L' || line[i] == 'u' || line[i] == 'U')) {
        ++i;
    }
    return i;
}

void emit(std::vector<Token>& out, std::size_t start, std::size_t length,
          TokenClass cls) {
    if (length == 0) return;
    out.push_back({start, length, cls});
}

} // namespace

// ── Public dispatch ───────────────────────────────────────────────────

std::vector<Token> tokenize_line(std::string_view line, CodeLanguage language) {
    switch (language) {
        case CodeLanguage::Cpp:        return tokenize_cpp_line(line);
        case CodeLanguage::JavaScript: return tokenize_javascript_line(line);
        case CodeLanguage::Python:     return tokenize_python_line(line);
        case CodeLanguage::JSON:       return tokenize_json_line(line);
        case CodeLanguage::Lua:        return tokenize_lua_line(line);
        case CodeLanguage::PlainText:
        case CodeLanguage::XML:
        case CodeLanguage::GLSL:
        case CodeLanguage::Faust:
        case CodeLanguage::JSFX:
        default: {
            std::vector<Token> out;
            if (!line.empty()) emit(out, 0, line.size(), TokenClass::text);
            return out;
        }
    }
}

bool is_keyword(std::string_view name, CodeLanguage language) {
    switch (language) {
        case CodeLanguage::Cpp:        return in_set(kCppKeywords, name);
        case CodeLanguage::JavaScript: return in_set(kJsKeywords, name);
        case CodeLanguage::Python:     return in_set(kPythonKeywords, name);
        case CodeLanguage::JSON:       return in_set(kJsonKeywords, name);
        case CodeLanguage::Lua:        return in_set(kLuaKeywords, name);
        default: return false;
    }
}

// ── C++ ───────────────────────────────────────────────────────────────

std::vector<Token> tokenize_cpp_line(std::string_view line) {
    std::vector<Token> out;
    std::size_t i = 0;

    // Skip leading whitespace and look for `#` as the first non-blank
    // character → preprocessor directive line.
    {
        std::size_t j = 0;
        while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
        if (j < line.size() && line[j] == '#') {
            emit(out, j, line.size() - j, TokenClass::preprocessor);
            return out;
        }
    }

    while (i < line.size()) {
        char c = line[i];

        // Line comment
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            emit(out, i, line.size() - i, TokenClass::comment);
            return out;
        }
        // Block comment opener (single-line scope)
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            std::size_t end = line.find("*/", i + 2);
            std::size_t len = (end == std::string_view::npos)
                                  ? line.size() - i
                                  : end - i + 2;
            emit(out, i, len, TokenClass::comment);
            i += len;
            continue;
        }
        // String / char literal
        if (c == '"' || c == '\'') {
            std::size_t end = scan_string(line, i);
            emit(out, i, end - i, TokenClass::string);
            i = end;
            continue;
        }
        // Number
        if (is_digit(c)) {
            std::size_t end = scan_number(line, i);
            emit(out, i, end - i, TokenClass::number);
            i = end;
            continue;
        }
        // Identifier / keyword
        if (is_id_start(c)) {
            std::size_t end = i + 1;
            while (end < line.size() && is_id_cont(line[end])) ++end;
            auto word = line.substr(i, end - i);
            TokenClass cls = TokenClass::text;
            if (in_set(kCppKeywords, word))      cls = TokenClass::keyword;
            else if (in_set(kCppTypes, word))    cls = TokenClass::type;
            if (cls != TokenClass::text) emit(out, i, end - i, cls);
            i = end;
            continue;
        }
        ++i;
    }
    return out;
}

// ── JavaScript ────────────────────────────────────────────────────────

std::vector<Token> tokenize_javascript_line(std::string_view line) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            emit(out, i, line.size() - i, TokenClass::comment);
            return out;
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            std::size_t end = line.find("*/", i + 2);
            std::size_t len = (end == std::string_view::npos)
                                  ? line.size() - i
                                  : end - i + 2;
            emit(out, i, len, TokenClass::comment);
            i += len;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            std::size_t end = scan_string(line, i);
            emit(out, i, end - i, TokenClass::string);
            i = end;
            continue;
        }
        if (is_digit(c)) {
            std::size_t end = scan_number(line, i);
            emit(out, i, end - i, TokenClass::number);
            i = end;
            continue;
        }
        if (is_id_start(c)) {
            std::size_t end = i + 1;
            while (end < line.size() && is_id_cont(line[end])) ++end;
            auto word = line.substr(i, end - i);
            TokenClass cls = TokenClass::text;
            if (in_set(kJsKeywords, word))      cls = TokenClass::keyword;
            else if (in_set(kJsTypes, word))    cls = TokenClass::type;
            if (cls != TokenClass::text) emit(out, i, end - i, cls);
            i = end;
            continue;
        }
        ++i;
    }
    return out;
}

// ── Python ────────────────────────────────────────────────────────────

std::vector<Token> tokenize_python_line(std::string_view line) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == '#') {
            emit(out, i, line.size() - i, TokenClass::comment);
            return out;
        }
        // Triple-quoted strings — emit the visible portion of the
        // opener as a string span; the editor re-tokenizes per line so
        // we cannot reasonably span the next lines from here.
        if ((c == '"' || c == '\'') && i + 2 < line.size()
            && line[i + 1] == c && line[i + 2] == c) {
            std::size_t end = line.find(std::string_view(&c, 1).data(), i + 3);
            // Simpler: search for the triple-quote terminator on the
            // same line; if it is not there, paint to end-of-line.
            std::string_view tq(&line[i], 3);
            std::size_t close = line.find(tq, i + 3);
            std::size_t len = (close == std::string_view::npos)
                                  ? line.size() - i
                                  : close - i + 3;
            emit(out, i, len, TokenClass::string);
            i += len;
            continue;
        }
        if (c == '"' || c == '\'') {
            std::size_t end = scan_string(line, i);
            emit(out, i, end - i, TokenClass::string);
            i = end;
            continue;
        }
        if (is_digit(c)) {
            std::size_t end = scan_number(line, i);
            emit(out, i, end - i, TokenClass::number);
            i = end;
            continue;
        }
        if (is_id_start(c)) {
            std::size_t end = i + 1;
            while (end < line.size() && is_id_cont(line[end])) ++end;
            auto word = line.substr(i, end - i);
            if (in_set(kPythonKeywords, word)) {
                emit(out, i, end - i, TokenClass::keyword);
            }
            i = end;
            continue;
        }
        ++i;
    }
    return out;
}

// ── JSON ──────────────────────────────────────────────────────────────

std::vector<Token> tokenize_json_line(std::string_view line) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == '"') {
            std::size_t end = scan_string(line, i);
            emit(out, i, end - i, TokenClass::string);
            i = end;
            continue;
        }
        if (is_digit(c) || (c == '-' && i + 1 < line.size()
                            && is_digit(line[i + 1]))) {
            std::size_t start = i;
            if (c == '-') ++i;
            std::size_t end = scan_number(line, i);
            emit(out, start, end - start, TokenClass::number);
            i = end;
            continue;
        }
        if (is_id_start(c)) {
            std::size_t end = i + 1;
            while (end < line.size() && is_id_cont(line[end])) ++end;
            auto word = line.substr(i, end - i);
            if (in_set(kJsonKeywords, word)) {
                emit(out, i, end - i, TokenClass::keyword);
            }
            i = end;
            continue;
        }
        ++i;
    }
    return out;
}

// ── Lua ───────────────────────────────────────────────────────────────

std::vector<Token> tokenize_lua_line(std::string_view line) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            emit(out, i, line.size() - i, TokenClass::comment);
            return out;
        }
        if (c == '"' || c == '\'') {
            std::size_t end = scan_string(line, i);
            emit(out, i, end - i, TokenClass::string);
            i = end;
            continue;
        }
        if (is_digit(c)) {
            std::size_t end = scan_number(line, i);
            emit(out, i, end - i, TokenClass::number);
            i = end;
            continue;
        }
        if (is_id_start(c)) {
            std::size_t end = i + 1;
            while (end < line.size() && is_id_cont(line[end])) ++end;
            auto word = line.substr(i, end - i);
            if (in_set(kLuaKeywords, word)) {
                emit(out, i, end - i, TokenClass::keyword);
            }
            i = end;
            continue;
        }
        ++i;
    }
    return out;
}

// ── Markdown ──────────────────────────────────────────────────────────

std::vector<Token> tokenize_markdown_line(std::string_view line) {
    std::vector<Token> out;
    if (line.empty()) return out;

    // Heading: 1–6 `#` followed by space.
    std::size_t lead = 0;
    while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
    if (lead < line.size() && line[lead] == '#') {
        std::size_t hashes = lead;
        while (hashes < line.size() && line[hashes] == '#'
               && hashes - lead < 6) {
            ++hashes;
        }
        if (hashes > lead && hashes < line.size() && line[hashes] == ' ') {
            emit(out, lead, line.size() - lead, TokenClass::heading);
            return out;
        }
    }

    // Inline code, links, and emphasis runs.
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        // Backtick code span
        if (c == '`') {
            std::size_t end = line.find('`', i + 1);
            std::size_t len = (end == std::string_view::npos)
                                  ? line.size() - i
                                  : end - i + 1;
            emit(out, i, len, TokenClass::string);
            i += len;
            continue;
        }
        // Link: [text](url) — emit the whole thing as `link`.
        if (c == '[') {
            std::size_t bracket = line.find(']', i + 1);
            if (bracket != std::string_view::npos
                && bracket + 1 < line.size()
                && line[bracket + 1] == '(') {
                std::size_t paren = line.find(')', bracket + 2);
                std::size_t end = (paren == std::string_view::npos)
                                      ? line.size()
                                      : paren + 1;
                emit(out, i, end - i, TokenClass::link);
                i = end;
                continue;
            }
        }
        ++i;
    }
    return out;
}

} // namespace pulp::view
