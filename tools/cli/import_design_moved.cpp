// import_design_moved.cpp - `pulp import-design` when the importer is absent.

#include "import_design_moved.hpp"

#include <iostream>
#include <set>

namespace pulp::cli {

namespace {

// Quote a word unless every character is plainly safe in a POSIX shell.
std::string shell_word(const std::string& value) {
    static const std::string safe = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "0123456789_./:=@%+,-";
    if (!value.empty() && value.find_first_not_of(safe) == std::string::npos)
        return value;
    std::string out = "'";
    for (char c : value) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
}

struct ParsedImport {
    std::string from;
    std::vector<std::string> files;
    std::string frame;
    std::string page;
    std::string output;
    std::string emit;
    std::string mode;
    std::string source_out;
    std::string manifest;
    std::string provenance;
    bool outline = false;
    bool validate_only = false;
    std::string unsupported; // the first argument Vellum's import cannot take
};

ParsedImport parse(const std::vector<std::string>& args) {
    ParsedImport p;
    for (size_t i = 0; i < args.size() && p.unsupported.empty(); ++i) {
        const auto& arg = args[i];
        const bool has_value = i + 1 < args.size();
        std::string* slot = nullptr;
        if (arg == "--from")
            slot = &p.from;
        else if (arg == "--frame")
            slot = &p.frame;
        else if (arg == "--page")
            slot = &p.page;
        else if (arg == "--output")
            slot = &p.output;
        else if (arg == "--emit")
            slot = &p.emit;
        else if (arg == "--mode")
            slot = &p.mode;
        else if (arg == "--source-out")
            slot = &p.source_out;
        else if (arg == "--manifest")
            slot = &p.manifest;
        else if (arg == "--provenance")
            slot = &p.provenance;
        if (slot != nullptr) {
            if (!has_value) {
                p.unsupported = arg;
            } else {
                *slot = args[++i];
            }
        } else if (arg == "--file" && has_value) {
            p.files.push_back(args[++i]);
        } else if (arg == "--outline") {
            p.outline = true;
        } else if (arg == "--validate-only") {
            p.validate_only = true;
        } else {
            p.unsupported = arg;
        }
    }
    return p;
}

// Why Vellum's grammar cannot express this invocation, or empty when it can.
std::string untranslatable(const ParsedImport& p) {
    if (!p.unsupported.empty())
        return p.unsupported;
    static const std::set<std::string> baked_sources = {"figma", "figma-plugin", "pencil",
                                                        "v0",    "designmd",     "design-ir"};
    const bool jsx = p.from == "jsx";
    const bool illustrator = p.from == "illustrator" || p.from == "ai" || p.from == "pdf";
    const bool baked_emit = p.emit == "cpp" || p.emit == "swiftui";
    if (baked_emit) {
        // `vellum import --emit cpp|swiftui` lowers one file of these sources.
        if (!baked_sources.count(p.from))
            return "--emit " + p.emit + " --from " + p.from;
        if (p.files.size() > 1)
            return "--file (more than one)";
        if (p.outline)
            return "--outline";
        if (!p.frame.empty())
            return "--frame";
        if (!p.page.empty())
            return "--page";
        if (p.validate_only)
            return "--validate-only";
        if (!p.source_out.empty())
            return "--source-out";
        if (!p.manifest.empty())
            return "--manifest";
        if (!p.provenance.empty())
            return "--provenance";
        return {};
    }
    if (jsx)
        return {};
    // A project import writes into the project, so only the Illustrator route
    // takes an output path, and only JSX takes an emit, mode or source tree.
    if (!p.output.empty() && !illustrator)
        return "--output";
    if (!p.emit.empty() && p.emit != "js")
        return "--emit " + p.emit;
    if (!p.mode.empty() && p.mode != "live")
        return "--mode " + p.mode;
    if (!p.source_out.empty())
        return "--source-out";
    if (p.validate_only && p.from != "preconverted-svg")
        return "--validate-only";
    if (!p.manifest.empty() && p.from != "preconverted-svg")
        return "--manifest";
    if (!p.provenance.empty() && !illustrator)
        return "--provenance";
    return {};
}

} // namespace

std::string unsupported_import_design_option(const std::vector<std::string>& args) {
    return untranslatable(parse(args));
}

std::string vellum_import_spelling(const std::vector<std::string>& args) {
    const auto p = parse(args);
    if (!untranslatable(p).empty())
        return {};
    const std::string file = p.files.empty() ? std::string("FILE") : shell_word(p.files.front());

    static const std::set<std::string> source_types = {"figma", "figma-plugin", "fig", "design-ir",
                                                       "html"};
    static const std::set<std::string> named_sources = {"pencil", "designmd", "v0", "jsx",
                                                        "preconverted-svg"};
    std::string spelling = "vellum import ";
    if (p.from.empty()) {
        spelling += file;
    } else if (p.from == "claude") {
        spelling += "--source-type claude-design " + file;
    } else if (source_types.count(p.from)) {
        spelling += "--source-type " + p.from + " " + file;
        for (size_t i = 1; i < p.files.size() && p.from == "figma-plugin"; ++i) {
            spelling += " --file " + shell_word(p.files[i]);
        }
    } else if (named_sources.count(p.from)) {
        spelling += p.from + " " + file;
    } else if (p.from == "illustrator" || p.from == "ai" || p.from == "pdf") {
        spelling += "illustrator " + file;
    } else {
        return {};
    }
    if (p.outline)
        spelling += " --outline";
    if (!p.page.empty())
        spelling += " --page " + shell_word(p.page);
    if (!p.frame.empty())
        spelling += " --frame " + shell_word(p.frame);
    if (p.validate_only)
        spelling += " --validate-only";
    if (!p.mode.empty())
        spelling += " --mode " + shell_word(p.mode);
    if (!p.emit.empty())
        spelling += " --emit " + shell_word(p.emit);
    if (!p.source_out.empty())
        spelling += " --source-out " + shell_word(p.source_out);
    if (!p.manifest.empty())
        spelling += " --manifest " + shell_word(p.manifest);
    if (!p.provenance.empty())
        spelling += " --provenance " + shell_word(p.provenance);
    if (!p.output.empty())
        spelling += " --out " + shell_word(p.output);
    return spelling;
}

int report_import_design_moved(const std::vector<std::string>& args) {
    std::cerr << "pulp import-design: the design importer is not built into this Pulp and now\n"
              << "lives in the Vellum CLI. Vellum imports into a Vellum project (see\n"
              << "`vellum create`) and writes canonical DesignIR, not ui.js.\n";
    const auto unsupported = unsupported_import_design_option(args);
    const auto spelling = vellum_import_spelling(args);
    if (!unsupported.empty()) {
        std::cerr << "  `vellum import` has no equivalent for " << unsupported << ".\n";
    } else if (spelling.empty()) {
        std::cerr << "  Vellum has no import route for this source yet.\n";
    } else {
        std::cerr << "  Equivalent: " << spelling << "\n";
    }
    return 2;
}

} // namespace pulp::cli
