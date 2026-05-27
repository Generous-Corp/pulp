#pragma once

/// @file code_editor_tokenizer.hpp
/// Lightweight per-language tokenizers for `CodeEditor` syntax
/// highlighting (closes the gap-doc Phase 4 row "CodeEditor language
/// coverage").
///
/// Each call to `tokenize_line(line, language)` walks a single source
/// line and returns a flat list of `Token` spans the editor can color.
/// The tokenizers are deliberately tiny — they are NOT full parsers,
/// just enough state to color keywords, strings, comments, numbers, and
/// preprocessor directives the way a typical IDE skim does. Multi-line
/// constructs (block comments, multi-line strings) are reported on the
/// first line only; the editor paints each line independently in the
/// existing native fallback paint path.
///
/// All tokenizers are stateless, allocation-free per token, and run in
/// O(n) over the input characters — safe to call on every paint without
/// caching. Headless-friendly: no platform / canvas dependencies.
///
/// License-lineage note: the tokenizer keyword lists are derived from
/// the official language references (ISO C++, ECMAScript, Python,
/// CommonMark, ECMA-404 JSON, Lua manual) — no copy from the reference
/// framework's tokeniser sources.

#include <cstddef>
#include <string_view>
#include <vector>

#include <pulp/view/code_editor.hpp>

namespace pulp::view {

/// Token classification for `tokenize_line`. The colors the editor
/// renders for each class are theme-driven; the tokenizer only labels
/// the span.
enum class TokenClass {
    text,        ///< Plain text (default color)
    keyword,     ///< Language keyword (e.g. `if`, `return`, `class`)
    type,        ///< Built-in / common type name (e.g. `int`, `Number`)
    identifier,  ///< Identifier — the tokenizer rarely emits this on
                 ///  its own; callers can treat unmatched text as
                 ///  identifier.
    number,      ///< Numeric literal (int / float / hex / bin)
    string,      ///< String / character literal (single or double quote)
    comment,     ///< Single-line comment (`//`, `#`, `--`) or the start
                 ///  of a block comment.
    preprocessor,///< Preprocessor / directive line (e.g. `#include`).
    punctuation, ///< Operators and structural punctuation.
    heading,     ///< Markdown heading (`#`, `##`, …).
    link,        ///< Markdown / hyperlink span.
};

/// One token span — half-open `[start, start+length)` into the line.
struct Token {
    std::size_t start = 0;
    std::size_t length = 0;
    TokenClass cls = TokenClass::text;
};

/// Tokenize a single line of source text for `language`. Returns an
/// ordered, non-overlapping list of spans. Characters not covered by a
/// span are treated as `TokenClass::text` by the editor.
///
/// `language` mappings:
///   - `Cpp`        → `tokenize_cpp_line`
///   - `JavaScript` → `tokenize_javascript_line`
///   - `Python`     → `tokenize_python_line`
///   - `JSON`       → `tokenize_json_line`
///   - `Lua`        → `tokenize_lua_line`
///   - everything else (including `PlainText`, `XML`, `GLSL`, `Faust`,
///     `JSFX`) → returns the line as a single `text` span.
///
/// Markdown does not have an entry in `CodeLanguage` yet — callers can
/// invoke `tokenize_markdown_line` directly for `*.md` content.
std::vector<Token> tokenize_line(std::string_view line, CodeLanguage language);

// ── Per-language entry points (callable independently) ────────────────

std::vector<Token> tokenize_cpp_line(std::string_view line);
std::vector<Token> tokenize_javascript_line(std::string_view line);
std::vector<Token> tokenize_python_line(std::string_view line);
std::vector<Token> tokenize_json_line(std::string_view line);
std::vector<Token> tokenize_lua_line(std::string_view line);
std::vector<Token> tokenize_markdown_line(std::string_view line);

/// True when `name` is one of the keywords the tokenizer recognises
/// for `language`. Useful for editor callers that want to do their own
/// scanning but reuse the keyword set.
bool is_keyword(std::string_view name, CodeLanguage language);

} // namespace pulp::view
