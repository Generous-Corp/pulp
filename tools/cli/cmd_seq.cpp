#include "cli_common.hpp"

#include <pulp/runtime/detail/durable_file_replacement.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using pulp::tools::timeline::OperationResult;
using pulp::tools::timeline::ProjectSource;

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

enum class ReadTextError { None, OpenFailed, TooLarge, ReadFailed };

struct ReadTextResult {
    std::optional<std::string> text;
    ReadTextError error = ReadTextError::None;
};

ReadTextResult read_text_bounded(const fs::path& path, std::size_t maximum_bytes) {
    std::error_code error;
    const auto file_bytes = fs::file_size(path, error);
    if (error)
        return {{}, ReadTextError::OpenFailed};
    if (file_bytes > maximum_bytes ||
        file_bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        file_bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        return {{}, ReadTextError::TooLarge};

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return {{}, ReadTextError::OpenFailed};
    std::string text;
    try {
        text.resize(static_cast<std::size_t>(file_bytes));
    } catch (...) {
        return {{}, ReadTextError::ReadFailed};
    }
    if (!text.empty()) {
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (stream.gcount() != static_cast<std::streamsize>(text.size()))
            return {{}, ReadTextError::ReadFailed};
    }
    char extra = 0;
    if (stream.read(&extra, 1))
        return {{},
                text.size() == maximum_bytes ? ReadTextError::TooLarge : ReadTextError::ReadFailed};
    if (!stream.eof())
        return {{}, ReadTextError::ReadFailed};
    return {std::move(text), ReadTextError::None};
}

enum class AtomicWriteOutcome {
    NotReplaced,
    ReplacedDurably,
    ReplacedButDirectorySyncFailed,
};

AtomicWriteOutcome write_text_atomic(const fs::path& destination, std::string_view text) noexcept {
    auto replacement = pulp::runtime::detail::DurableFileReplacement::create(destination);
    if (!replacement)
        return AtomicWriteOutcome::NotReplaced;
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    if (!replacement->write_all(bytes))
        return AtomicWriteOutcome::NotReplaced;
    switch (replacement->commit()) {
    case pulp::runtime::detail::DurableFileCommitOutcome::NotReplaced:
        return AtomicWriteOutcome::NotReplaced;
    case pulp::runtime::detail::DurableFileCommitOutcome::ReplacedDurably:
        return AtomicWriteOutcome::ReplacedDurably;
    case pulp::runtime::detail::DurableFileCommitOutcome::ReplacedButDirectorySyncFailed:
        return AtomicWriteOutcome::ReplacedButDirectorySyncFailed;
    }
    return AtomicWriteOutcome::NotReplaced;
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
        const auto project = pulp::tools::timeline::filesystem_path_from_utf8(args[1]);
        return emit(pulp::tools::timeline::validate(ProjectSource::file(project)));
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
        const auto project = pulp::tools::timeline::filesystem_path_from_utf8(args[1]);
        return emit(pulp::tools::timeline::explain(ProjectSource::file(project), sample_rate));
    }

    if (subcommand == "apply") {
        if (args.size() != 3 && args.size() != 5)
            return bad_seq_usage("apply requires project and command JSON paths");
        const auto project_path = pulp::tools::timeline::filesystem_path_from_utf8(args[1]);
        const auto command_path = pulp::tools::timeline::filesystem_path_from_utf8(args[2]);
        const auto maximum_command_bytes = pulp::timeline::DecodeLimits{}.max_input_bytes;
        const auto commands = read_text_bounded(command_path, maximum_command_bytes);
        if (!commands.text) {
            if (commands.error == ReadTextError::TooLarge) {
                std::cerr << "pulp seq: command file exceeds " << maximum_command_bytes
                          << " bytes: " << args[2] << "\n";
                return 1;
            }
            std::cerr << "pulp seq: could not read command file: " << args[2] << "\n";
            return 1;
        }
        fs::path output;
        if (args.size() == 5) {
            if (args[3] != "--out")
                return bad_seq_usage("unknown apply option: " + args[3]);
            output = pulp::tools::timeline::filesystem_path_from_utf8(args[4]);
        }
        auto result = pulp::tools::timeline::command_apply(ProjectSource::file(project_path),
                                                           *commands.text);
        if (!result || output.empty())
            return emit(std::move(result));
        const auto project = project_member(result.json);
        if (project && has_package_relative_locator(*project) &&
            !same_parent_directory(project_path, output)) {
            std::cerr << "pulp seq: --out cannot move a project with package-relative media; "
                         "write beside the input project\n";
            return 2;
        }
        if (!project) {
            std::cerr << "pulp seq: could not write project: " << args[4] << "\n";
            return 1;
        }
        const auto written = write_text_atomic(output, *project);
        if (written == AtomicWriteOutcome::NotReplaced) {
            std::cerr << "pulp seq: could not write project: " << args[4] << "\n";
            return 1;
        }
        if (written == AtomicWriteOutcome::ReplacedButDirectorySyncFailed) {
            std::cerr << "pulp seq: project was replaced at " << args[4]
                      << ", but its parent directory could not be synchronized; durability is "
                         "uncertain\n";
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
            output = pulp::tools::timeline::filesystem_path_from_utf8(args[index]);
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
            project = pulp::tools::timeline::filesystem_path_from_utf8(args[index]);
        } else {
            std::cerr << "pulp render: unexpected argument: " << args[index] << "\n";
            return 2;
        }
    }
    if (project.empty() || output.empty()) {
        print_render_usage();
        return 2;
    }
    return emit(pulp::tools::timeline::render(ProjectSource::file(project), output, sample_rate));
}
