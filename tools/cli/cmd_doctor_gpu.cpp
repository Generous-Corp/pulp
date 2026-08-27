#include "cli_common.hpp"

#include <pulp_tooling/gpu_health/health_provider.hpp>

#include <iostream>
#include <string>
#include <vector>

int cmd_doctor_gpu(const std::vector<std::string>& args) {
    bool json = false;
    bool no_render = false;
    for (const auto& arg : args) {
        if (arg == "--json") json = true;
        else if (arg == "--no-render") no_render = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: pulp doctor gpu [--no-render] [--json]\n\n"
                         "Runs bounded Renderer3D, Skia/Graphite, and GpuCompute "
                         "render/readback oracles.\n"
                         "  --no-render  Report inventory as unverified without "
                         "acquiring a GPU device\n"
                         "  --json       Emit pulp.gpu-health-result.v1 JSON\n";
            return 0;
        } else {
            std::cerr << "pulp doctor gpu: unknown option '" << arg << "'\n"
                      << "Usage: pulp doctor gpu [--no-render] [--json]\n";
            return 2;
        }
    }

    auto provider = pulp::tooling::gpu_health::make_default_health_provider();
    auto result = pulp::tooling::gpu_health::run_health_check(*provider, !no_render);
    std::string validation_error;
    if (!pulp::tooling::gpu_health::validate(result, &validation_error)) {
        std::cerr << "pulp doctor gpu: internal result validation failed: "
                  << validation_error << "\n";
        return 1;
    }

    if (json)
        std::cout << pulp::tooling::gpu_health::to_json(result, true) << '\n';
    else
        std::cout << pulp::tooling::gpu_health::render_human(result);
    return pulp::tooling::gpu_health::exit_code(result);
}
