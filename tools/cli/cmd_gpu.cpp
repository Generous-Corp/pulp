#include "cli_common.hpp"

#include <pulp_tooling/gpu_probe/recipes.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using pulp::tooling::gpu_probe::ArtifactPayload;
using pulp::tooling::gpu_probe::RecipeRun;
using pulp::tooling::gpu_probe::RunOptions;

constexpr const char* kUsage =
    "Usage: pulp gpu probe --recipe <id> --artifacts <dir> [--negative-control] [--json]\n";

bool path_chain_contains_symlink(const fs::path& path) {
    fs::path cursor;
    for (const auto& part : fs::absolute(path).lexically_normal()) {
        cursor /= part;
        std::error_code ec;
        const auto status = fs::symlink_status(cursor, ec);
        if (!ec && fs::is_symlink(status)) return true;
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw fs::filesystem_error("inspect artifact path", cursor, ec);
        }
    }
    return false;
}

void write_payload(const fs::path& directory,
                   const std::string& evidence_id,
                   const ArtifactPayload& payload) {
    const fs::path relative{payload.artifact.name};
    if (relative.empty() || relative.is_absolute() || relative.has_parent_path() ||
        relative.filename() != relative) {
        throw std::runtime_error("probe returned an unsafe artifact name");
    }

    const auto destination = directory / relative;
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(destination, ec))) {
        throw std::runtime_error("refusing to replace symlink artifact: " + destination.string());
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        throw fs::filesystem_error("inspect artifact destination", destination, ec);
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = directory /
        ("." + relative.string() + ".tmp." + evidence_id + "." + std::to_string(nonce));
    struct TempCleanup {
        fs::path path;
        ~TempCleanup() {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    } cleanup{temporary};

    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot create artifact: " + temporary.string());
    stream.write(reinterpret_cast<const char*>(payload.bytes.data()),
                 static_cast<std::streamsize>(payload.bytes.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot write artifact: " + temporary.string());

    fs::rename(temporary, destination, ec);
    if (ec) throw fs::filesystem_error("publish artifact", temporary, destination, ec);
    cleanup.path.clear();
}

void write_artifacts(const fs::path& directory, const RecipeRun& run) {
    if (path_chain_contains_symlink(directory)) {
        throw std::runtime_error("artifact directory or parent is a symlink");
    }
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) throw fs::filesystem_error("create artifact directory", directory, ec);
    if (!fs::is_directory(directory)) {
        throw std::runtime_error("artifact path is not a directory: " + directory.string());
    }
    for (const auto& payload : run.payloads) {
        write_payload(directory, run.result.gpu_evidence_id, payload);
    }
}

RecipeRun run_recipe(const std::string& recipe_id, const RunOptions& options) {
    if (recipe_id == "renderer3d.hardcoded-cube.v1") {
        return pulp::tooling::gpu_probe::run_renderer3d_recipe(options);
    }
    if (recipe_id == "gpu-compute.magnitude.v1") {
        return pulp::tooling::gpu_probe::run_gpu_compute_magnitude_recipe(options);
    }
    throw std::runtime_error("recipe is registered but not available in this build: " + recipe_id);
}

int cmd_gpu_probe(const std::vector<std::string>& args) {
    bool json = false;
    RunOptions options;
    std::string recipe_id;
    fs::path artifact_directory;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--json") json = true;
        else if (arg == "--negative-control") options.apply_negative_mutation = true;
        else if (arg == "--recipe" && i + 1 < args.size()) recipe_id = args[++i];
        else if (arg == "--artifacts" && i + 1 < args.size()) artifact_directory = args[++i];
        else if (arg == "--help" || arg == "-h") {
            std::cout << kUsage << "\n"
                      << "Runs a bounded native recipe and writes its declared evidence artifacts.\n"
                      << "  --negative-control  Apply the recipe's seeded mutation; success means it is detected\n"
                      << "  --json              Emit pulp.gpu-probe-result.v1 JSON\n\n"
                      << "Recipes:\n";
            for (const auto& recipe : pulp::tooling::gpu_probe::recipes())
                std::cout << "  " << recipe.id << '\n';
            return 0;
        } else {
            std::cerr << "pulp gpu probe: unknown or incomplete option '" << arg << "'\n" << kUsage;
            return 2;
        }
    }

    if (recipe_id.empty() || artifact_directory.empty()) {
        std::cerr << "pulp gpu probe: --recipe and --artifacts are required\n" << kUsage;
        return 2;
    }
    if (!pulp::tooling::gpu_probe::find_recipe(recipe_id)) {
        std::cerr << "pulp gpu probe: unknown recipe '" << recipe_id << "'\n";
        return 2;
    }

    try {
        auto run = run_recipe(recipe_id, options);
        std::string validation_error;
        if (!pulp::tooling::gpu_probe::validate(run, &validation_error)) {
            std::cerr << "pulp gpu probe: internal result validation failed: "
                      << validation_error << '\n';
            return 1;
        }
        write_artifacts(artifact_directory, run);
        if (json) std::cout << pulp::tooling::gpu_probe::to_json(run.result, true) << '\n';
        else std::cout << pulp::tooling::gpu_probe::render_human(run.result);
        return pulp::tooling::gpu_probe::exit_code(run.result);
    } catch (const std::exception& error) {
        std::cerr << "pulp gpu probe: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

int cmd_gpu(const std::vector<std::string>& args) {
    if (args.empty() || args.front() == "--help" || args.front() == "-h") {
        std::cout << "Usage: pulp gpu <command> [options]\n\n"
                     "Commands:\n"
                     "  probe  Run a deterministic GPU evidence recipe\n";
        return 0;
    }
    if (args.front() != "probe") {
        std::cerr << "pulp gpu: unknown subcommand '" << args.front() << "'\n"
                     "Usage: pulp gpu probe --recipe <id> --artifacts <dir> [options]\n";
        return 2;
    }
    return cmd_gpu_probe({args.begin() + 1, args.end()});
}
