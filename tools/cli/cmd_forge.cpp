// cmd_forge.cpp — installed Forge catalog discovery.

#include "cli_common.hpp"

#include <pulp/host/forge_catalog_export.hpp>

#include <fstream>
#include <iostream>

namespace {

void print_forge_usage() {
    std::cout << "pulp forge — Forge node catalog\n\n"
              << "Usage: pulp forge catalog export --json [--output <path>]\n\n"
              << "Exports every Forge node, realization, axis, and parameter.\n"
              << "The JSON joins semantic descriptors to factory-owned numeric "
                 "ranges.\n";
}

} // namespace

int cmd_forge(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_forge_usage();
        return 0;
    }
    if (args.size() < 3 || args[0] != "catalog" || args[1] != "export") {
        std::cerr << "Error: expected `pulp forge catalog export --json`\n\n";
        print_forge_usage();
        return 1;
    }

    bool json = false;
    fs::path output;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--output") {
            if (++i == args.size() || args[i].empty()) {
                std::cerr << "Error: --output requires a path\n";
                return 1;
            }
            output = args[i];
        } else if (args[i] == "--help" || args[i] == "-h") {
            print_forge_usage();
            return 0;
        } else {
            std::cerr << "Error: unknown `pulp forge catalog export` option: " << args[i] << "\n";
            return 1;
        }
    }
    if (!json) {
        std::cerr << "Error: `pulp forge catalog export` requires --json\n";
        return 1;
    }

    auto exported = pulp::host::export_forge_catalog_json();
    if (!exported.ok) {
        std::cerr << "Error: " << exported.error << "\n";
        return 1;
    }
    if (output.empty()) {
        std::cout << exported.json;
        return 0;
    }

    std::error_code error;
    const auto parent = output.parent_path();
    if (!parent.empty())
        fs::create_directories(parent, error);
    if (error) {
        std::cerr << "Error: cannot create output directory: " << error.message() << "\n";
        return 1;
    }
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream || !(stream << exported.json)) {
        std::cerr << "Error: cannot write " << output.string() << "\n";
        return 1;
    }
    return 0;
}
