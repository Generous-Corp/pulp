#include "cmd_graph_internal.hpp"

#include <choc/text/choc_JSON.h>
#include <iostream>

namespace pulp_cli::graph_detail {

std::string patch_code(Code code) {
    return pulp::host::graph_patch_code_name(code);
}

Code file_load_code(const pulp::host::GraphSerializer::LoadResult& loaded) {
    if (loaded.error.starts_with("graph file exceeds "))
        return Code::ResourceLimit;
    return loaded.error.starts_with("graph file ") ? Code::FileUnreadable : Code::InvalidDocument;
}

void json_error(const std::string& code, const std::string& message) {
    auto value = choc::value::createObject("GraphError");
    value.addMember("valid", false);
    value.addMember("code", code);
    value.addMember("message", message);
    std::cout << choc::json::toString(value) << '\n';
}

std::optional<std::string> read_graph_document(const std::string& path, Code& code,
                                               std::string& message) {
    std::string text;
    if (!pulp::host::GraphSerializer::read_file_bounded(path, text, message)) {
        // The serializer reader names the cause: the byte limit is a resource
        // limit, anything else (missing, changed while reading) is unreadable.
        code =
            message.starts_with("graph file exceeds") ? Code::ResourceLimit : Code::FileUnreadable;
        return std::nullopt;
    }
    return text;
}

} // namespace pulp_cli::graph_detail
