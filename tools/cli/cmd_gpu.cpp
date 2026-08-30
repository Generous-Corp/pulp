#include "atomic_text_file.hpp"
#include "cli_common.hpp"
#include "gpu_artifact_publication.hpp"
#include "json_parser.hpp"
#include "json_writer.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp_tooling/gpu_probe/recipes.hpp>
#if PULP_ENABLE_PROJECT_PACKAGE
#include <pulp/project_package/atomic_publisher.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using pulp::tooling::gpu_probe::RecipeRun;
using pulp::tooling::gpu_probe::RunOptions;

constexpr const char* kUsage =
    "Usage: pulp gpu probe --recipe <id> --artifacts <dir> [--negative-control] [--json]\n";

using JsonValue = pulp::cli::pkg::JsonValue;

bool path_chain_contains_symlink(const fs::path& path);

bool gpu_probe_test_fault(std::string_view name) {
#if PULP_CLI_GPU_PROBE_TEST_FAULTS
    const auto* fault = std::getenv("PULP_GPU_PROBE_TEST_FAULT");
    return fault != nullptr && name == fault;
#else
    (void)name;
    return false;
#endif
}

std::optional<std::string> secure_gpu_evidence_id() {
    if (auto bytes = pulp::runtime::secure_random_bytes(16); bytes && bytes->size() == 16)
        return pulp::runtime::hex_encode(*bytes);
    return std::nullopt;
}

std::string unverified_gpu_evidence_id() {
    // An entropy-provider failure must not erase the typed exit-2 diagnostic.
    // This process-local nonce is correlation only and never supports a pass.
    static std::atomic<std::uint64_t> sequence{0};
    std::ostringstream seed;
    seed << std::chrono::system_clock::now().time_since_epoch().count() << ':'
         << std::chrono::steady_clock::now().time_since_epoch().count() << ':'
         << sequence.fetch_add(1, std::memory_order_relaxed) << ':'
         << reinterpret_cast<std::uintptr_t>(&sequence);
    return pulp::runtime::sha256_hex(seed.str()).substr(0, 32);
}

pulp::tooling::gpu_probe::ProbeResult make_unverified_command_result(
    const pulp::tooling::gpu_probe::RecipeDefinition& recipe, const RunOptions& options,
    std::string_view code, std::string_view detail) {
    using namespace pulp::tooling::gpu_probe;

    ProbeResult result;
    result.gpu_evidence_id = options.gpu_evidence_id ? *options.gpu_evidence_id
                                                     : unverified_gpu_evidence_id();
    result.recipe_id = std::string(recipe.id);
    result.source_digest = pulp::runtime::sha256_hex(recipe.source_identity);
    std::ostringstream signature;
    signature << "pulp.gpu-probe-command-unverified.v1\n" << recipe.id << '\n'
              << result.source_digest << '\n' << recipe.dimensions.width << 'x'
              << recipe.dimensions.height << ':' << recipe.dimensions.work_items << '\n'
              << recipe.seed << '\n' << recipe.clock << '\n' << recipe.input_format << '\n'
              << recipe.output_format << '\n' << recipe.encoding << '\n'
              << recipe.tolerance.absolute << ':' << recipe.tolerance.relative << '\n'
              << static_cast<int>(recipe.adapter_policy) << '\n'
              << (options.apply_negative_mutation ? recipe.negative_mutation
                                                  : recipe.positive_control);
    result.signature_digest = pulp::runtime::sha256_hex(signature.str());
    result.dimensions = recipe.dimensions;
    if (options.apply_negative_mutation && recipe.id == kRecipeIds.front())
        result.dimensions = {32, 32, 1'024};
    result.seed = recipe.seed;
    result.clock = std::string(recipe.clock);
    result.input_format = std::string(recipe.input_format);
    result.output_format = std::string(recipe.output_format);
    result.encoding = std::string(recipe.encoding);
    result.tolerance = recipe.tolerance;
    result.adapter_policy = recipe.adapter_policy;
    if (options.apply_negative_mutation)
        result.mutation = std::string(recipe.negative_mutation);
    result.verdict = Verdict::unverified;
    for (std::uint32_t index = 0; index < recipe.semantic_passes.size(); ++index) {
        PassResult pass;
        pass.sequence = index;
        pass.name = std::string(recipe.semantic_passes[index]);
        pass.verdict = Verdict::unverified;
        pass.work_completed = false;
        pass.code = std::string(code);
        result.passes.push_back(std::move(pass));
    }
    const auto message = detail.empty()
        ? std::string{"GPU probe command failed before evidence was verified."}
        : std::string{detail.substr(0, 512)};
    result.recommendations.push_back(message);
    return result;
}

int report_probe_command_failure(bool json,
                                 const pulp::tooling::gpu_probe::RecipeDefinition& recipe,
                                 const RunOptions& options, std::string_view label,
                                 std::string_view code, std::string_view detail) {
    std::cerr << "pulp gpu probe: " << label;
    if (!detail.empty())
        std::cerr << ": " << detail;
    std::cerr << '\n';
    if (json) {
        try {
            const auto result = make_unverified_command_result(recipe, options, code, detail);
            std::string validation_error;
            if (!pulp::tooling::gpu_probe::validate(result, &validation_error)) {
                std::cerr << "pulp gpu probe: could not encode typed command failure: "
                          << validation_error << '\n';
            } else {
                std::cout << pulp::tooling::gpu_probe::to_json(result, true) << '\n';
            }
        } catch (const std::exception& error) {
            std::cerr << "pulp gpu probe: could not encode typed command failure: " << error.what()
                      << '\n';
        }
    }
    return 2;
}

bool path_has_dot_component(const fs::path& path) {
    for (const auto& part : path)
        if (part == "." || part == "..")
            return true;
    return false;
}

std::string serialize_json(const JsonValue& value) {
    using pulp::cli::json_string;
    switch (value.type) {
    case JsonValue::Null:
        return "null";
    case JsonValue::Bool:
        return value.bool_val ? "true" : "false";
    case JsonValue::Number: {
        std::ostringstream out;
        out << value.num_val;
        return out.str();
    }
    case JsonValue::String:
        return json_string(value.str_val);
    case JsonValue::Array: {
        std::string out{"["};
        for (std::size_t index = 0; index < value.arr().size(); ++index) {
            if (index != 0)
                out += ',';
            out += serialize_json(value.arr()[index]);
        }
        return out + ']';
    }
    case JsonValue::Object: {
        std::string out{"{"};
        for (std::size_t index = 0; index < value.obj().size(); ++index) {
            if (index != 0)
                out += ',';
            out += json_string(value.obj()[index].first) + ':' +
                   serialize_json(value.obj()[index].second);
        }
        return out + '}';
    }
    }
    return "null";
}

const JsonValue& recipe_catalog() {
    static const JsonValue catalog = [] {
        std::string text{pulp::tooling::gpu_probe::recipe_catalog_json()};
        pulp::cli::pkg::JsonParser parser{text};
        auto parsed = parser.parse();
        const auto* schema = parsed.get("schema");
        const auto* recipes = parsed.get("recipes");
        if (parsed.type != JsonValue::Object || !schema || schema->type != JsonValue::String ||
            schema->str_val != "pulp.gpu-recipes.v1" || !recipes ||
            recipes->type != JsonValue::Array)
            throw std::runtime_error("embedded GPU recipe catalog is malformed");
        return parsed;
    }();
    return catalog;
}

const JsonValue* find_catalog_recipe(const std::string& id) {
    for (const auto& recipe : recipe_catalog().get("recipes")->arr()) {
        const auto* candidate = recipe.get("id");
        if (candidate && candidate->type == JsonValue::String && candidate->str_val == id)
            return &recipe;
    }
    return nullptr;
}

bool recipe_matches_symptom(const JsonValue& recipe, const std::string& symptom) {
    if (symptom.empty())
        return true;
    const auto* symptoms = recipe.get("symptoms");
    if (!symptoms || symptoms->type != JsonValue::Array)
        return false;
    for (const auto& value : symptoms->arr())
        if (value.type == JsonValue::String && value.str_val == symptom)
            return true;
    return false;
}

bool recipe_is_callable(const JsonValue& recipe) {
    const auto* id = recipe.get("id");
    return id && id->type == JsonValue::String &&
           pulp::tooling::gpu_probe::is_recipe_callable(id->str_val);
}

std::string discovery_json(const std::vector<const JsonValue*>& recipes) {
    using pulp::cli::json_string;
    const auto* revision = recipe_catalog().get("catalog_revision");
    std::string out = "{\"schema\":\"pulp.gpu-recipes-discovery.v1\",\"catalog_revision\":" +
                      (revision ? serialize_json(*revision) : "null") + ",\"recipes\":[";
    for (std::size_t index = 0; index < recipes.size(); ++index) {
        if (index != 0)
            out += ',';
        out += "{\"callable\":";
        out += recipe_is_callable(*recipes[index]) ? "true" : "false";
        out += ",\"recipe\":" + serialize_json(*recipes[index]) + '}';
    }
    return out + "]}";
}

std::string string_field(const JsonValue& object, const char* key) {
    const auto* value = object.get(key);
    return value && value->type == JsonValue::String ? value->str_val : std::string{};
}

int cmd_gpu_recipes_list(const std::vector<std::string>& args) {
    bool json = false;
    bool symptom_supplied = false;
    std::string symptom;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--json")
            json = true;
        else if (args[index] == "--symptom" && index + 1 < args.size()) {
            symptom_supplied = true;
            symptom = args[++index];
        } else {
            std::cerr << "pulp gpu recipes list: unknown or incomplete option '" << args[index]
                      << "'\n";
            return 2;
        }
    }
    if (symptom_supplied && symptom.empty()) {
        std::cerr << "pulp gpu recipes list: --symptom must be non-empty\n";
        return 2;
    }
    std::vector<const JsonValue*> matches;
    for (const auto& recipe : recipe_catalog().get("recipes")->arr())
        if (recipe_matches_symptom(recipe, symptom))
            matches.push_back(&recipe);
    if (symptom_supplied && matches.empty()) {
        std::cerr << "pulp gpu recipes list: unknown symptom '" << symptom << "'\n";
        return 2;
    }
    if (json) {
        std::cout << discovery_json(matches) << '\n';
        return 0;
    }
    for (const auto* recipe : matches) {
        std::cout << string_field(*recipe, "id") << "  "
                  << (recipe_is_callable(*recipe) ? "callable" : "conditional") << "  "
                  << string_field(*recipe, "summary") << '\n';
    }
    return 0;
}

int cmd_gpu_recipes_show(const std::vector<std::string>& args) {
    bool json = false;
    std::string id;
    for (const auto& arg : args) {
        if (arg == "--json")
            json = true;
        else if (id.empty())
            id = arg;
        else {
            std::cerr << "pulp gpu recipes show: expected one recipe id\n";
            return 2;
        }
    }
    const auto* recipe = find_catalog_recipe(id);
    if (!recipe) {
        std::cerr << "pulp gpu recipes show: unknown recipe '" << id << "'\n";
        return 2;
    }
    if (json) {
        std::cout << discovery_json({recipe}) << '\n';
        return 0;
    }
    std::cout << string_field(*recipe, "id") << '\n'
              << "  " << string_field(*recipe, "title") << '\n'
              << "  availability: " << (recipe_is_callable(*recipe) ? "callable" : "conditional")
              << '\n'
              << "  " << string_field(*recipe, "summary") << '\n';
    return 0;
}

int cmd_gpu_recipes_scaffold(const std::vector<std::string>& args) {
    bool json = false;
    std::string id;
    fs::path output;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--json")
            json = true;
        else if (args[index] == "--output" && index + 1 < args.size())
            output = args[++index];
        else if (id.empty())
            id = args[index];
        else {
            std::cerr << "pulp gpu recipes scaffold: unknown or incomplete argument '"
                      << args[index] << "'\n";
            return 2;
        }
    }
    const auto* recipe = find_catalog_recipe(id);
    if (!recipe || output.empty() || !output.is_absolute()) {
        std::cerr << "pulp gpu recipes scaffold: a known recipe id and --output are required\n";
        if (!output.empty() && !output.is_absolute())
            std::cerr << "pulp gpu recipes scaffold: --output must be absolute\n";
        return 2;
    }
    try {
        if (path_has_dot_component(output))
            throw std::runtime_error("output path must not contain '.' or '..' components");
        output = output.lexically_normal();
        if (output.filename().empty())
            output = output.parent_path();
        if (path_chain_contains_symlink(output))
            throw std::runtime_error("output directory or parent is a symlink");
#if !PULP_ENABLE_PROJECT_PACKAGE
        throw std::runtime_error("recipe scaffolding requires project-package support");
#else
        auto publisher = pulp::project_package::AtomicPublisher::create(output);
        if (!publisher)
            throw std::runtime_error(
                "output must not exist and must have a safe existing parent directory");
        const auto callable = recipe_is_callable(*recipe);
        const auto* revision = recipe_catalog().get("catalog_revision");
        const std::string receipt =
            "{\"schema\":\"pulp.gpu-recipe-selection.v1\",\"catalog_schema\":"
            "\"pulp.gpu-recipes.v1\",\"catalog_revision\":" +
            (revision ? serialize_json(*revision) : "null") +
            ",\"recipe\":" + serialize_json(*recipe) + "}\n";
        const std::string readme =
            "# " + id +
            " GPU evidence workspace\n\n"
            "`gpu-recipe.json` is a selection receipt and catalog snapshot, not a new authority. "
            "The canonical catalog is the matched SDK's `share/pulp/gpu-recipes.yaml` (or "
            "`docs/status/gpu-recipes.yaml` in a source checkout). Run the baseline twice, "
            "then run its seeded negative control. Exit 0 is verified pass, exit 1 is a completed "
            "measured failure, and exit 2 is unavailable or unverified. Fix the seeded failure "
            "by removing only `--negative-control` and rerunning the same catalog command; the "
            "input and independent oracle must remain unchanged.\n\n"
            "```sh\n"
            "pulp gpu probe --recipe " +
            id +
            " --artifacts \"$PWD/artifacts/baseline-1\" --json\n"
            "pulp gpu probe --recipe " +
            id +
            " --artifacts \"$PWD/artifacts/baseline-2\" --json\n"
            "pulp gpu probe --recipe " +
            id +
            " --artifacts \"$PWD/artifacts/negative\" --negative-control --json\n"
            "```\n";
        std::error_code ec;
        fs::create_directory(publisher->staging_directory() / "artifacts", ec);
        if (ec || !publisher->write("gpu-recipe.json", receipt) ||
            !publisher->write("README.md", readme))
            throw std::runtime_error("could not stage the recipe workspace");
        const auto publication = publisher->commit_directory();
        if (!publication ||
            publication.value() != pulp::project_package::AtomicPublishOutcome::PublishedDurably)
            throw std::runtime_error("could not publish the recipe workspace durably");
        if (json)
            std::cout << "{\"schema\":\"pulp.gpu-recipe-scaffold-result.v1\",\"recipe_id\":"
                      << pulp::cli::json_string(id)
                      << ",\"callable\":" << (callable ? "true" : "false") << ",\"output\":"
                      << pulp::cli::json_string(fs::absolute(output).lexically_normal().string())
                      << "}\n";
        else
            std::cout << "Created GPU recipe workspace at " << output << '\n';
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "pulp gpu recipes scaffold: " << error.what() << '\n';
        return 1;
    }
}

int cmd_gpu_recipes(const std::vector<std::string>& args) {
    if (args.empty() || args.front() == "--help" || args.front() == "-h") {
        std::cout << "Usage: pulp gpu recipes <list|show|scaffold> [options]\n\n"
                     "  list [--symptom <token>] [--json]\n"
                     "  show <recipe-id> [--json]\n"
                     "  scaffold <recipe-id> --output <new-dir> [--json]\n";
        return 0;
    }
    const std::vector<std::string> tail(args.begin() + 1, args.end());
    if (args.front() == "list")
        return cmd_gpu_recipes_list(tail);
    if (args.front() == "show")
        return cmd_gpu_recipes_show(tail);
    if (args.front() == "scaffold")
        return cmd_gpu_recipes_scaffold(tail);
    std::cerr << "pulp gpu recipes: unknown subcommand '" << args.front() << "'\n";
    return 2;
}

bool path_chain_contains_symlink(const fs::path& path) {
    fs::path cursor;
    for (const auto& part : fs::absolute(path)) {
        cursor /= part;
        std::error_code ec;
        const auto status = fs::symlink_status(cursor, ec);
        if (!ec && fs::is_symlink(status))
            return true;
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw fs::filesystem_error("inspect artifact path", cursor, ec);
        }
    }
    return false;
}

void write_artifacts(const fs::path& directory, const RecipeRun& run) {
    auto pinned = pulp::cli::gpu_artifacts::PinnedArtifactDirectory::open_or_create(directory);
    for (const auto& payload : run.payloads)
        pinned.publish(payload.artifact.name, payload.bytes);
}

RecipeRun run_recipe(const std::string& recipe_id, const RunOptions& options,
                     const std::optional<std::string>& threejs_runtime_root) {
    if (recipe_id == "renderer3d.hardcoded-cube.v1") {
        return pulp::tooling::gpu_probe::run_renderer3d_recipe(options);
    }
    if (recipe_id == "gpu-compute.magnitude.v1") {
        return pulp::tooling::gpu_probe::run_gpu_compute_magnitude_recipe(options);
    }
    if (recipe_id == "gpu-audio.stft.v1") {
        return pulp::tooling::gpu_probe::run_gpu_audio_stft_recipe(options);
    }
    if (recipe_id == "threejs.multi-pass.v1") {
        return pulp::tooling::gpu_probe::run_threejs_multi_pass_recipe(options,
                                                                       threejs_runtime_root);
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
        if (arg == "--json")
            json = true;
        else if (arg == "--negative-control")
            options.apply_negative_mutation = true;
        else if (arg == "--recipe" && i + 1 < args.size())
            recipe_id = args[++i];
        else if (arg == "--artifacts" && i + 1 < args.size())
            artifact_directory = args[++i];
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << kUsage << "\n"
                << "Runs a bounded native recipe and writes its declared evidence artifacts.\n"
                << "  --negative-control  Apply the recipe's seeded mutation; success means it is "
                   "detected\n"
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
    const auto* recipe = pulp::tooling::gpu_probe::find_recipe(recipe_id);
    if (!recipe) {
        std::cerr << "pulp gpu probe: unknown recipe '" << recipe_id << "'\n";
        return 2;
    }

    try {
        const auto evidence_id = secure_gpu_evidence_id();
        if (!evidence_id) {
            return report_probe_command_failure(
                json, *recipe, options, "runtime failure", "gpu_evidence_id_unavailable",
                "GPU probe could not allocate a unique evidence identifier");
        }
        options.gpu_evidence_id = *evidence_id;
        if (gpu_probe_test_fault("runtime"))
            throw std::runtime_error("injected GPU probe runtime failure");
        const auto executable_dir = current_executable_path().parent_path();
        const std::array runtime_candidates{
            executable_dir / "share" / "pulp" / "threejs",
            executable_dir.parent_path() / "share" / "pulp" / "threejs",
        };
        std::optional<std::string> threejs_runtime_root;
        std::error_code runtime_path_error;
        for (const auto& candidate : runtime_candidates) {
            if (fs::is_directory(candidate, runtime_path_error)) {
                threejs_runtime_root = candidate.string();
                break;
            }
            runtime_path_error.clear();
        }
        if (!threejs_runtime_root) {
            auto build_root = executable_dir;
            bool build_tree_invocation = false;
            for (int depth = 0; depth < 4 && !build_root.empty(); ++depth) {
                if (fs::is_regular_file(build_root / "CMakeCache.txt", runtime_path_error)) {
                    build_tree_invocation = true;
                    break;
                }
                runtime_path_error.clear();
                build_root = build_root.parent_path();
            }
            if (!build_tree_invocation)
                threejs_runtime_root = runtime_candidates.front().string();
        }
        auto run = run_recipe(recipe_id, options, threejs_runtime_root);
        if (gpu_probe_test_fault("result-validation"))
            run.result.schema.clear();
        std::string validation_error;
        if (!pulp::tooling::gpu_probe::validate(run, &validation_error)) {
            return report_probe_command_failure(json, *recipe, options,
                                                "internal result validation failed",
                                                "probe_result_validation_failed",
                                                validation_error);
        }
        try {
            write_artifacts(artifact_directory, run);
        } catch (const std::exception& error) {
            return report_probe_command_failure(json, *recipe, options,
                                                "artifact publication failed",
                                                "artifact_publication_failed", error.what());
        }
        if (json)
            std::cout << pulp::tooling::gpu_probe::to_json(run.result, true) << '\n';
        else
            std::cout << pulp::tooling::gpu_probe::render_human(run.result);
        return pulp::tooling::gpu_probe::exit_code(run.result);
    } catch (const std::exception& error) {
        return report_probe_command_failure(json, *recipe, options, "runtime failure",
                                            "probe_runtime_failed", error.what());
    }
}

} // namespace

int cmd_gpu(const std::vector<std::string>& args) {
    if (args.empty() || args.front() == "--help" || args.front() == "-h") {
        std::cout << "Usage: pulp gpu <command> [options]\n\n"
                     "Commands:\n"
                     "  probe    Run a deterministic GPU evidence recipe\n"
                     "  recipes  Discover and scaffold canonical recipe workflows\n";
        return 0;
    }
    if (args.front() == "recipes")
        return cmd_gpu_recipes({args.begin() + 1, args.end()});
    if (args.front() != "probe") {
        std::cerr << "pulp gpu: unknown subcommand '" << args.front()
                  << "'\n"
                     "Usage: pulp gpu probe --recipe <id> --artifacts <dir> [options]\n";
        return 2;
    }
    return cmd_gpu_probe({args.begin() + 1, args.end()});
}
