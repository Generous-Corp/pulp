#include "cli_common.hpp"
#include "atomic_text_file.hpp"

#include <pulp/host/forge_catalog_json.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

namespace {

void print_forge_usage() {
    std::cout << "pulp forge — Forge integration data\n\n";
    std::cout << "Usage: pulp forge catalog export [--json | --check | --write]\n\n";
    std::cout << "  --json    Emit the joined semantic catalog to stdout\n";
    std::cout << "  --check   Verify the installed-artifact snapshot is current\n";
    std::cout << "  --write   Regenerate docs/status/forge-catalog.json\n";
}

int render_findings(const std::vector<pulp::host::ForgeAuditFinding>& findings) {
    for (const auto& finding : findings)
        std::cerr << pulp::host::describe_forge_audit_finding(finding) << "\n";
    return findings.empty() ? 0 : 1;
}

} // namespace

int cmd_forge(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_forge_usage();
        return 0;
    }
    if (args.size() < 2 || args[0] != "catalog" || args[1] != "export") {
        std::cerr << "Error: expected `pulp forge catalog export`\n\n";
        print_forge_usage();
        return 2;
    }

    std::string mode = "--json";
    if (args.size() == 3)
        mode = args[2];
    if (args.size() > 3 || (mode != "--json" && mode != "--check" && mode != "--write")) {
        std::cerr << "Error: unknown Forge catalog export option\n\n";
        print_forge_usage();
        return 2;
    }

    const auto nodes = pulp::host::forge_catalog_export_nodes();
    const auto findings = pulp::host::audit_forge_catalog_export(nodes);
    if (render_findings(findings) != 0)
        return 1;
    const auto json = pulp::host::serialize_forge_catalog_json(nodes);

    if (mode == "--json") {
        std::cout << json;
        return 0;
    }

    auto root = require_project_root();
    if (!root)
        return 1;
    const auto snapshot = *root / "docs" / "status" / "forge-catalog.json";

    if (mode == "--write") {
        std::string error;
        if (!pulp::cli::write_text_file_atomically(snapshot, json, error)) {
            std::cerr << "Error: " << error << "\n";
            return 1;
        }
        std::cout << "Wrote " << snapshot << "\n";
        return 0;
    }

    std::ifstream in(snapshot, std::ios::binary);
    std::ostringstream current;
    current << in.rdbuf();
    if (!in || current.str() != json) {
        std::cerr << "Forge catalog snapshot is stale: " << snapshot << "\n";
        std::cerr << "Run `pulp forge catalog export --write`.\n";
        return 1;
    }
    std::cout << "Forge catalog snapshot is current\n";
    return 0;
}
