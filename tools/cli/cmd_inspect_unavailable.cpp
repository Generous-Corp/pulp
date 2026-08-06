#include "inspector_shipping_report.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int cmd_inspect(const std::vector<std::string>& args) {
    if (!args.empty() && args.front() == "audit") {
        std::filesystem::path artifact;
        bool json_output = false;
        for (std::size_t index = 1; index < args.size(); ++index) {
            if (args[index] == "--json") {
                json_output = true;
            } else if (args[index] == "--help" || args[index] == "-h") {
                std::cout << "Usage: pulp inspect audit ARTIFACT [--json]\n"
                             "Verify the manifest, profile, digest, and linked "
                             "control surfaces without activating the artifact.\n";
                return 0;
            } else if (args[index].starts_with("-")) {
                std::cerr << "Error: unknown inspect audit argument: "
                          << args[index] << "\n";
                return 2;
            } else if (!artifact.empty()) {
                std::cerr << "Error: inspect audit accepts exactly one artifact\n";
                return 2;
            } else {
                artifact = args[index];
            }
        }
        if (artifact.empty()) {
            std::cerr << "Error: inspect audit requires ARTIFACT\n";
            return 2;
        }
        const auto report =
            pulp::cli::inspector_shipping::audit_artifact(artifact);
        std::cout << (json_output
            ? pulp::cli::inspector_shipping::audit_json(report) + "\n"
            : pulp::cli::inspector_shipping::audit_human(report));
        return report.complete ? 0 : 1;
    }
    std::cerr
        << "Error: this Pulp SDK was built without the optional development "
           "inspector component (PULP_ENABLE_INSPECTOR=OFF). Offline "
           "'pulp inspect audit' remains available.\n";
    return 1;
}
