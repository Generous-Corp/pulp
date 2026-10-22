#pragma once

// RuntimeEffectCache — compile-and-cache SkRuntimeEffect shaders by source hash.
// Lives at process lifetime (static), NOT on SkiaCanvas (which is recreated every frame).

#ifdef PULP_HAS_SKIA

#include "include/effects/SkRuntimeEffect.h"
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace pulp::canvas {

namespace runtime_effect_lint {

struct Token {
    std::string_view text;
    bool identifier = false;
    int line = 1;
};

inline bool is_statement_keyword(std::string_view word) {
    return word == "return" || word == "else" || word == "do" || word == "case";
}

// Identifiers, numbers, and single punctuation characters, with comments
// removed. Enough structure to find `main` and the declarations in it.
inline std::vector<Token> tokenize(std::string_view src) {
    std::vector<Token> out;
    int line = 1;
    size_t i = 0;
    auto ident_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    while (i < src.size()) {
        const char c = src[i];
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n')
                    ++line;
                ++i;
            }
            i += 2;
            continue;
        }
        if (ident_char(c)) {
            const size_t start = i;
            while (i < src.size() && (ident_char(src[i]) || src[i] == '.')) {
                if (src[i] == '.' && !std::isdigit(static_cast<unsigned char>(src[start])))
                    break;
                ++i;
            }
            const bool identifier = !std::isdigit(static_cast<unsigned char>(src[start]));
            out.push_back({src.substr(start, i - start), identifier, line});
            continue;
        }
        out.push_back({src.substr(i, 1), false, line});
        ++i;
    }
    return out;
}

} // namespace runtime_effect_lint

class RuntimeEffectCache {
public:
    static RuntimeEffectCache& instance() {
        static RuntimeEffectCache cache;
        return cache;
    }

    // Compile and cache an SkSL shader. Returns nullptr on failure.
    // Thread-safe via mutex (one compilation at a time).
    sk_sp<SkRuntimeEffect> get_or_compile(const std::string& sksl) {
        std::string ignored;
        return get_or_compile(sksl, ignored);
    }

    // Compile and cache an SkSL shader, returning the error text for THIS
    // compile via `error_out` (empty on success). The error is a local of this
    // call, never a cache-global: a previous version stored it in a shared
    // `last_error_` member written between two lock scopes, so a concurrent
    // compile of a different shader could overwrite it and a caller reading it
    // afterwards would attribute another shader's error to its own. Returning
    // the error from the call removes that race entirely.
    sk_sp<SkRuntimeEffect> get_or_compile(const std::string& sksl,
                                          std::string& error_out) {
        error_out.clear();
        auto hash = std::hash<std::string>{}(sksl);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = effects_.find(hash);
            if (it != effects_.end()) return it->second;
        }

        error_out = graphite_coordinate_name_hazard(sksl);
        if (!error_out.empty())
            return nullptr;

        // Compile outside lock (may be slow).
        auto result = SkRuntimeEffect::MakeForShader(SkString(sksl.c_str()));
        if (!result.effect) {
            error_out = result.errorText.c_str();
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        effects_[hash] = result.effect;
        return result.effect;
    }

    // Graphite emits a runtime shader's `main` as a helper whose coordinate
    // parameter is named `pos`, renaming the author's parameter but not the
    // author's locals. A local `pos` declared anywhere in `main` therefore
    // either redefines the parameter (the pipeline fails to compile and the
    // draw is dropped; on some Skia versions the null pipeline crashes inside
    // insertRecording) or, in a nested block, shadows the coordinates and
    // renders wrong pixels. The raster backend compiles the same source as
    // written, so the defect is backend-dependent and silent until it reaches
    // a GPU. Such a source is refused on every backend with this diagnostic;
    // returns empty when the source has no such local. Globals, helper
    // functions, and a `main` whose own parameter is named `pos` are
    // unaffected and accepted.
    static std::string graphite_coordinate_name_hazard(std::string_view sksl) {
        const auto tokens = runtime_effect_lint::tokenize(sksl);
        size_t depth = 0;
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            const auto& t = tokens[i];
            if (t.text == "{")
                ++depth;
            else if (t.text == "}" && depth > 0)
                --depth;
            if (depth != 0 || t.text != "main" || tokens[i + 1].text != "(")
                continue;

            // The coordinate parameter is the last identifier before `)`.
            size_t j = i + 2;
            std::string_view param;
            for (; j < tokens.size() && tokens[j].text != ")"; ++j)
                if (tokens[j].identifier)
                    param = tokens[j].text;
            if (j + 1 >= tokens.size() || tokens[j + 1].text != "{")
                continue;
            if (param == "pos")
                return {};

            // `parens` holds the token before each open `(`: a comma inside a
            // call or constructor separates arguments, while a comma at
            // statement level or in a `for` header separates declarators.
            size_t body_depth = 0;
            std::vector<std::string_view> parens;
            for (size_t k = j + 1; k < tokens.size(); ++k) {
                if (tokens[k].text == "{")
                    ++body_depth;
                else if (tokens[k].text == "}" && --body_depth == 0)
                    break;
                else if (tokens[k].text == "(")
                    parens.push_back(tokens[k - 1].text);
                else if (tokens[k].text == ")" && !parens.empty())
                    parens.pop_back();
                if (tokens[k].text != "pos" || k + 1 >= tokens.size())
                    continue;
                const auto& prev = tokens[k - 1];
                const auto& next = tokens[k + 1].text;
                const bool declarator = next == "=" || next == ";" || next == "," || next == "[";
                const bool after_type =
                    (prev.identifier && !runtime_effect_lint::is_statement_keyword(prev.text)) ||
                    prev.text == "]";
                const bool in_declarator_list = parens.empty() || parens.back() == "for";
                const bool after_comma = prev.text == "," && in_declarator_list;
                if (declarator && (after_type || after_comma)) {
                    return "line " + std::to_string(tokens[k].line) +
                           ": a local named 'pos' in main() collides with the coordinate "
                           "parameter Graphite gives a runtime shader (it fails to compile "
                           "or shadows the coordinates on the GPU); rename the local, or "
                           "name main's parameter 'pos'";
                }
            }
            return {};
        }
        return {};
    }

    // Clear all cached effects (for hot reload)
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        effects_.clear();
    }

private:
    RuntimeEffectCache() = default;
    std::unordered_map<size_t, sk_sp<SkRuntimeEffect>> effects_;
    std::mutex mutex_;
};

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
