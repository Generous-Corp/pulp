#include "cli_common.hpp"

#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using pulp::tools::timeline::OperationResult;

void print_seq_usage() {
    std::cout << "Usage: pulp seq <subcommand> [args]\n\n"
                 "Subcommands:\n"
                 "  schema\n"
                 "  validate <project.json>\n"
                 "  explain <project.json> [--sample-rate <hz>]\n"
                 "  apply <project.json> <commands.json> [--out <project.json>]\n";
}

void print_render_usage() {
    std::cout << "Usage: pulp render <project.json> --out <file.wav> "
                 "[--sample-rate <hz>]\n";
}

int emit(OperationResult result) {
    auto& stream = result ? std::cout : std::cerr;
    stream << result.json << "\n";
    return result.exit_code;
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool write_text_atomic(const fs::path& destination, std::string_view text) {
    auto temporary = destination;
    temporary +=
        ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    bool complete = false;
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (stream) {
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            complete = static_cast<bool>(stream);
        }
    }
    if (!complete) {
        std::error_code error;
        fs::remove(temporary, error);
        return false;
    }
    std::error_code error;
#ifdef _WIN32
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
#else
    fs::rename(temporary, destination, error);
    if (!error)
        return true;
#endif
    fs::remove(temporary, error);
    return false;
}

std::optional<std::uint32_t> parse_sample_rate(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > pulp::timebase::kMaximumCompiledSampleRate)
        return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

std::optional<std::string> project_member(std::string_view response) {
    auto parsed = pulp::timeline::parse_json(response);
    if (!parsed)
        return std::nullopt;
    const auto* project = parsed.value()->root().find("project");
    if (!project)
        return std::nullopt;
    return std::string(parsed.value()->raw(*project));
}

bool has_package_relative_locator(std::string_view project_json) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return true;
    auto project = pulp::timeline::deserialize_project(project_json, registry.value());
    if (!project)
        return true;
    for (const auto& asset : project.value().assets()) {
        for (const auto& locator : asset.locators)
            if (locator.kind == pulp::timeline::AssetLocatorKind::PackageRelative)
                return true;
        for (const auto& representation : asset.representations)
            for (const auto& locator : representation.locators)
                if (locator.kind == pulp::timeline::AssetLocatorKind::PackageRelative)
                    return true;
    }
    return false;
}

std::optional<fs::path> normalized_parent(const fs::path& path) {
    std::error_code error;
    auto absolute = fs::absolute(path, error);
    if (error)
        return std::nullopt;
    auto parent = absolute.lexically_normal().parent_path();
    auto canonical = fs::weakly_canonical(parent, error);
    return error ? std::optional<fs::path>{parent} : std::optional<fs::path>{canonical};
}

bool same_parent_directory(const fs::path& left, const fs::path& right) {
    const auto left_parent = normalized_parent(left);
    const auto right_parent = normalized_parent(right);
    if (!left_parent || !right_parent)
        return false;
    std::error_code error;
    const bool equivalent = fs::equivalent(*left_parent, *right_parent, error);
    return error ? *left_parent == *right_parent : equivalent;
}

int bad_seq_usage(std::string_view message) {
    std::cerr << "pulp seq: " << message << "\n\n";
    print_seq_usage();
    return 2;
}

} // namespace

int cmd_seq(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_seq_usage();
        return 0;
    }

    const auto& subcommand = args[0];
    if (subcommand == "schema") {
        if (args.size() != 1)
            return bad_seq_usage("schema accepts no arguments");
        return emit(pulp::tools::timeline::schema());
    }

    if (subcommand == "validate") {
        if (args.size() != 2)
            return bad_seq_usage("validate requires one project path");
        return emit(pulp::tools::timeline::validate(args[1]));
    }

    if (subcommand == "explain") {
        if (args.size() != 2 && args.size() != 4)
            return bad_seq_usage("explain requires a project path");
        std::uint32_t sample_rate = 48'000;
        if (args.size() == 4) {
            if (args[2] != "--sample-rate")
                return bad_seq_usage("unknown explain option: " + args[2]);
            const auto parsed = parse_sample_rate(args[3]);
            if (!parsed)
                return bad_seq_usage("--sample-rate must be between 1 and 768000");
            sample_rate = *parsed;
        }
        return emit(pulp::tools::timeline::explain(args[1], sample_rate));
    }

    if (subcommand == "apply") {
        if (args.size() != 3 && args.size() != 5)
            return bad_seq_usage("apply requires project and command JSON paths");
        const auto commands = read_text(args[2]);
        if (!commands) {
            std::cerr << "pulp seq: could not read command file: " << args[2] << "\n";
            return 1;
        }
        fs::path output;
        if (args.size() == 5) {
            if (args[3] != "--out")
                return bad_seq_usage("unknown apply option: " + args[3]);
            output = args[4];
        }
        auto result = pulp::tools::timeline::command_apply(args[1], *commands);
        if (!result || output.empty())
            return emit(std::move(result));
        const auto project = project_member(result.json);
        if (project && has_package_relative_locator(*project) &&
            !same_parent_directory(args[1], output)) {
            std::cerr << "pulp seq: --out cannot move a project with package-relative media; "
                         "write beside the input project\n";
            return 2;
        }
        if (!project || !write_text_atomic(output, *project)) {
            std::cerr << "pulp seq: could not write project: " << output << "\n";
            return 1;
        }
        return emit(std::move(result));
    }

    return bad_seq_usage("unknown subcommand: " + subcommand);
}

int cmd_render(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        print_render_usage();
        return 0;
    }

    fs::path project;
    fs::path output;
    std::uint32_t sample_rate = 48'000;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (args[index] == "--out") {
            if (++index == args.size()) {
                std::cerr << "pulp render: --out requires a path\n";
                return 2;
            }
            output = args[index];
        } else if (args[index] == "--sample-rate") {
            if (++index == args.size()) {
                std::cerr << "pulp render: --sample-rate requires a value\n";
                return 2;
            }
            const auto parsed = parse_sample_rate(args[index]);
            if (!parsed) {
                std::cerr << "pulp render: --sample-rate must be between 1 and 768000\n";
                return 2;
            }
            sample_rate = *parsed;
        } else if (args[index].starts_with("-")) {
            std::cerr << "pulp render: unknown option: " << args[index] << "\n";
            return 2;
        } else if (project.empty()) {
            project = args[index];
        } else {
            std::cerr << "pulp render: unexpected argument: " << args[index] << "\n";
            return 2;
        }
    }
    if (project.empty() || output.empty()) {
        print_render_usage();
        return 2;
    }
    return emit(pulp::tools::timeline::render(project.string(), output.string(), sample_rate));
}
