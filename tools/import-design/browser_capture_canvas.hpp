#pragma once

#include <string>

namespace pulp::import_design::detail {

// Internal CSS evidence only. This does not establish producer binding or
// authorize painting a root; those decisions belong to the capture importer.
struct DocumentCanvasPaint {
    std::string color;
    double width;
    double height;
};

} // namespace pulp::import_design::detail
