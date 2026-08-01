#include "overflow_selector.hpp"

#include <choc/text/choc_JSON.h>

#include <cctype>
#include <string_view>

namespace pulp::cli {
namespace {

class StrictSelectorSyntax {
  public:
    explicit StrictSelectorSyntax(const std::string& text) : text_(text) {}

    bool parse() {
        skip_space();
        const bool valid = peek() == '"' ? parse_string() : parse_array();
        skip_space();
        return valid && pos_ == text_.size();
    }

  private:
    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    void skip_space() {
        while (pos_ < text_.size() &&
               std::string_view{" \t\r\n"}.find(text_[pos_]) != std::string_view::npos)
            ++pos_;
    }

    bool parse_string() {
        if (peek() != '"')
            return false;
        ++pos_;
        while (pos_ < text_.size()) {
            const auto byte = static_cast<unsigned char>(text_[pos_++]);
            if (byte == '"')
                return true;
            if (byte < 0x20)
                return false;
            if (byte != '\\')
                continue;
            if (pos_ == text_.size())
                return false;
            const char escape = text_[pos_++];
            if (std::string_view{"\"\\/bfnrt"}.find(escape) != std::string_view::npos)
                continue;
            if (escape != 'u' || pos_ + 4 > text_.size())
                return false;
            for (int digit = 0; digit < 4; ++digit)
                if (!std::isxdigit(static_cast<unsigned char>(text_[pos_++])))
                    return false;
        }
        return false;
    }

    bool parse_array() {
        if (peek() != '[')
            return false;
        ++pos_;
        skip_space();
        if (!parse_string())
            return false;
        skip_space();
        while (peek() == ',') {
            ++pos_;
            skip_space();
            if (!parse_string())
                return false;
            skip_space();
        }
        if (peek() != ']')
            return false;
        ++pos_;
        return true;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

bool has_strict_selector_syntax(const std::string& text) {
    return StrictSelectorSyntax(text).parse();
}

bool safe_label(const std::string& label) {
    if (label.empty())
        return false;
    for (const unsigned char c : label) {
        const bool ascii_alnum =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(ascii_alnum || c == '_' || c == '.' || c == ':' || c == '-'))
            return false;
    }
    return true;
}

} // namespace

bool is_safe_runner_label(const std::string& label) {
    return safe_label(label);
}

OverflowSelectorValidation validate_overflow_selector(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    if (first != std::string::npos && text.substr(first, last - first + 1) == "local-only")
        return {false, true, "`local-only` disables overflow"};

    if (!has_strict_selector_syntax(text))
        return {false, false, "selector is not strict JSON"};

    try {
        // CHOC's parser expects a container at the top level. The strict
        // grammar above already proves that `text` is one complete selector,
        // so wrapping is only used to decode JSON escapes here.
        auto document = choc::json::parse("[" + text + "]");
        auto selector = document[0];
        if (selector.isString()) {
            const auto label = std::string(selector.getString());
            if (!is_safe_runner_label(label))
                return {false, false, "selector labels must use only A-Z, a-z, 0-9, _, ., :, or -"};
            return {label != "local-only", label == "local-only",
                    label == "local-only" ? "`local-only` disables overflow" : ""};
        }
        if (!selector.isArray() || selector.size() == 0)
            return {false, false, "selector must be a non-empty string or string array"};
        bool off_switch = false;
        for (uint32_t i = 0; i < selector.size(); ++i) {
            if (!selector[i].isString())
                return {false, false, "selector array must contain only strings"};
            const auto label = std::string(selector[i].getString());
            if (!is_safe_runner_label(label))
                return {false, false, "selector labels must use only A-Z, a-z, 0-9, _, ., :, or -"};
            off_switch = off_switch || label == "local-only";
        }
        if (off_switch)
            return {false, true, "`local-only` disables overflow"};
        return {true, false, ""};
    } catch (...) {
        return {false, false, "selector is not valid JSON"};
    }
}

} // namespace pulp::cli
