// import_design_moved.cpp - `pulp import-design` when the importer is absent.

#include "import_design_moved.hpp"

#include <iostream>
#include <set>

namespace pulp::cli {

namespace {

std::string shell_word(const std::string& value) {
    if (value.find_first_of(" \t'\"\\$`") == std::string::npos && !value.empty()) return value;
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

}  // namespace

std::string vellum_import_spelling(const std::vector<std::string>& args) {
    std::string from;
    std::vector<std::string> files;
    std::string frame;
    std::string page;
    bool outline = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--from" && has_value) from = args[++i];
        else if (arg == "--file" && has_value) files.push_back(args[++i]);
        else if (arg == "--frame" && has_value) frame = args[++i];
        else if (arg == "--page" && has_value) page = args[++i];
        else if (arg == "--outline") outline = true;
    }
    const std::string file = files.empty() ? std::string("FILE") : shell_word(files.front());

    static const std::set<std::string> source_types = {
        "figma", "figma-plugin", "fig", "design-ir", "html"};
    static const std::set<std::string> named_sources = {
        "pencil", "designmd", "v0", "jsx", "preconverted-svg"};
    std::string spelling = "vellum import ";
    if (from.empty()) {
        spelling += file;
    } else if (from == "claude") {
        spelling += "--source-type claude-design " + file;
    } else if (source_types.count(from)) {
        spelling += "--source-type " + from + " " + file;
        for (size_t i = 1; i < files.size() && from == "figma-plugin"; ++i) {
            spelling += " --file " + shell_word(files[i]);
        }
    } else if (named_sources.count(from)) {
        spelling += from + " " + file;
    } else if (from == "illustrator" || from == "ai" || from == "pdf") {
        spelling += "illustrator " + file;
    } else {
        return {};
    }
    if (outline) spelling += " --outline";
    if (!page.empty()) spelling += " --page " + shell_word(page);
    if (!frame.empty()) spelling += " --frame " + shell_word(frame);
    return spelling;
}

int report_import_design_moved(const std::vector<std::string>& args) {
    std::cerr << "pulp import-design: the design importer is not built into this Pulp and now\n"
              << "lives in the Vellum CLI. Vellum imports into a Vellum project (see\n"
              << "`vellum create`) and writes canonical DesignIR, not ui.js.\n";
    const auto spelling = vellum_import_spelling(args);
    if (spelling.empty()) {
        std::cerr << "  Vellum has no import route for this source yet.\n";
    } else {
        std::cerr << "  Equivalent: " << spelling << "\n";
    }
    return 2;
}

}  // namespace pulp::cli
