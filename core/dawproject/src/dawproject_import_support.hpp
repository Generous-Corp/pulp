#pragma once

#include <pulp/timeline/dawproject_import.hpp>

#include <pugixml.hpp>

#include <optional>
#include <string>

namespace pulp::timeline::detail {

DawProjectImportError import_error(DawProjectImportErrorCode code, std::string message);

bool has_attribute(const pugi::xml_node& node, const char* name);
bool parse_number(const pugi::xml_attribute& attribute, double& out);
bool parse_number(const pugi::xml_attribute& attribute, long long& out);

std::optional<DawProjectImportError> require_double(const pugi::xml_node& node, const char* name,
                                                    const char* context, double& out);
std::optional<DawProjectImportError> require_int(const pugi::xml_node& node, const char* name,
                                                 const char* context, long long& out);

} // namespace pulp::timeline::detail
