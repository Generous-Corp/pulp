// import_design_moved.hpp - `pulp import-design` when the importer is absent.
#pragma once

#include <string>
#include <vector>

namespace pulp::cli {

// The `vellum import` spelling for a `pulp import-design` invocation, or an
// empty string when Vellum has no route for the requested source or cannot
// express one of its arguments. Every argument is translated or refused, so
// the spelling never silently drops an input or an option.
std::string vellum_import_spelling(const std::vector<std::string>& args);

// The first argument (or argument combination) `vellum import` cannot
// express, or an empty string when the whole invocation translates.
std::string unsupported_import_design_option(const std::vector<std::string>& args);

// Explain that the importer moved to `vellum import`, print the equivalent
// spelling, and return 2. Arguments are not forwarded: Vellum imports into a
// Vellum project with a different grammar and output contract.
int report_import_design_moved(const std::vector<std::string>& args);

} // namespace pulp::cli
