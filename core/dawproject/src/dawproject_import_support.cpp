#include "dawproject_import_support.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <clocale>
#else
#include <locale.h>
#endif

namespace pulp::timeline::detail {

DawProjectImportError import_error(DawProjectImportErrorCode code, std::string message) {
    return DawProjectImportError{code, std::move(message), {}};
}

bool has_attribute(const pugi::xml_node& node, const char* name) {
    return !node.attribute(name).empty();
}

bool parse_number(const pugi::xml_attribute& attribute, double& out) {
    const std::string buffer(attribute.as_string());
    errno = 0;
    char* end = nullptr;
    double value = 0.0;
#if defined(_WIN32)
    static _locale_t c_locale = ::_create_locale(LC_ALL, "C");
    if (c_locale == nullptr)
        return false;
    value = ::_strtod_l(buffer.c_str(), &end, c_locale);
#else
    static ::locale_t c_locale = ::newlocale(LC_ALL_MASK, "C", static_cast<::locale_t>(0));
    if (c_locale == static_cast<::locale_t>(0))
        return false;
    const ::locale_t previous = ::uselocale(c_locale);
    value = std::strtod(buffer.c_str(), &end);
    ::uselocale(previous);
#endif
    if (errno == ERANGE || end == buffer.c_str() || end != buffer.c_str() + buffer.size())
        return false;
    out = value;
    return true;
}

bool parse_number(const pugi::xml_attribute& attribute, long long& out) {
    const std::string_view text = attribute.as_string();
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

std::optional<DawProjectImportError> require_double(const pugi::xml_node& node, const char* name,
                                                    const char* context, double& out) {
    const auto attribute = node.attribute(name);
    if (attribute.empty())
        return import_error(DawProjectImportErrorCode::MissingAttribute,
                            std::string(context) + " is missing required attribute '" + name + "'");
    if (!parse_number(attribute, out))
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            std::string(context) + " has invalid numeric attribute '" + name + "'");
    return std::nullopt;
}

std::optional<DawProjectImportError> require_int(const pugi::xml_node& node, const char* name,
                                                 const char* context, long long& out) {
    const auto attribute = node.attribute(name);
    if (attribute.empty())
        return import_error(DawProjectImportErrorCode::MissingAttribute,
                            std::string(context) + " is missing required attribute '" + name + "'");
    if (!parse_number(attribute, out))
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            std::string(context) + " has invalid integer attribute '" + name + "'");
    return std::nullopt;
}

} // namespace pulp::timeline::detail
