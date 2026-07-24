#include "cli_common.hpp"

#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/tools/timeline/agent.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
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

fs::path temporary_path(const fs::path& destination, std::uint64_t serial) {
    auto temporary = destination;
#ifdef _WIN32
    const auto process = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    const auto process = static_cast<std::uint64_t>(::getpid());
#endif
    temporary += ".tmp." + std::to_string(process) + "." + std::to_string(serial);
    return temporary;
}

void remove_temporary(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
}

#ifndef _WIN32
bool sync_file(int descriptor) noexcept {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (::fcntl(descriptor, F_FULLFSYNC) == 0)
        return true;
    if (errno != ENOTSUP)
        return false;
#endif
    return ::fsync(descriptor) == 0;
}

bool sync_parent_directory(const fs::path& destination) noexcept {
    auto parent = destination.parent_path();
    if (parent.empty())
        parent = ".";
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
}
#endif

#if defined(__APPLE__)
bool clear_extended_acl(int descriptor) noexcept {
    acl_t empty = ::acl_init(0);
    errno = 0;
    const bool cleared =
        empty != nullptr && ::acl_set_fd_np(descriptor, empty, ACL_TYPE_EXTENDED) == 0;
    const bool unsupported = !cleared && errno == EOPNOTSUPP;
    if (empty != nullptr)
        ::acl_free(empty);
    return cleared || unsupported;
}
#endif

bool write_text_atomic(const fs::path& destination, std::string_view text) noexcept {
    static std::atomic<std::uint64_t> next_serial{1};
    fs::path temporary;
#ifdef _WIN32
    constexpr SECURITY_INFORMATION kSecurityInformation =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    std::vector<std::uint8_t> security_descriptor;
    SECURITY_ATTRIBUTES security_attributes{};
    SECURITY_ATTRIBUTES* security = nullptr;
    const auto destination_attributes = ::GetFileAttributesW(destination.c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES) {
        DWORD required = 0;
        ::GetFileSecurityW(destination.c_str(), kSecurityInformation, nullptr, 0, &required);
        if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return false;
        security_descriptor.resize(required);
        if (!::GetFileSecurityW(destination.c_str(), kSecurityInformation,
                                security_descriptor.data(), required, &required))
            return false;
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.lpSecurityDescriptor = security_descriptor.data();
        security = &security_attributes;
    } else if (::GetLastError() != ERROR_FILE_NOT_FOUND &&
               ::GetLastError() != ERROR_PATH_NOT_FOUND) {
        return false;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt != 128; ++attempt) {
        temporary = temporary_path(destination, next_serial.fetch_add(1));
        handle = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, security, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
            break;
        if (::GetLastError() != ERROR_FILE_EXISTS && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    if (handle == INVALID_HANDLE_VALUE)
        return false;

    bool complete = true;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(text.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!::WriteFile(handle, text.data() + offset, chunk, &written, nullptr) || written == 0) {
            complete = false;
            break;
        }
        offset += written;
    }
    complete = complete && ::FlushFileBuffers(handle) != 0;
    complete = ::CloseHandle(handle) != 0 && complete;
    if (!complete) {
        remove_temporary(temporary);
        return false;
    }
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
#else
    bool destination_exists = false;
    struct stat destination_status{};
    if (::lstat(destination.c_str(), &destination_status) == 0) {
        if (!S_ISREG(destination_status.st_mode))
            return false;
        destination_exists = true;
    } else if (errno != ENOENT) {
        return false;
    }
#if defined(__APPLE__)
    acl_t destination_acl = nullptr;
    if (destination_exists) {
        destination_acl = ::acl_get_file(destination.c_str(), ACL_TYPE_EXTENDED);
        if (destination_acl == nullptr && errno != ENOENT && errno != ENOATTR)
            return false;
        if (destination_acl != nullptr) {
            acl_entry_t entry = nullptr;
            const auto entry_result = ::acl_get_entry(destination_acl, ACL_FIRST_ENTRY, &entry);
            if (entry_result < 0) {
                ::acl_free(destination_acl);
                return false;
            }
            if (entry_result == 0) {
                ::acl_free(destination_acl);
                destination_acl = nullptr;
            }
        }
    }
#elif defined(__linux__)
    std::optional<std::vector<std::uint8_t>> destination_acl;
    if (destination_exists) {
        constexpr auto acl_name = "system.posix_acl_access";
        errno = 0;
        const auto acl_size = ::getxattr(destination.c_str(), acl_name, nullptr, 0);
        if (acl_size < 0) {
            if (errno != ENODATA)
                return false;
        } else {
            destination_acl.emplace(static_cast<std::size_t>(acl_size));
            const auto copied =
                ::getxattr(destination.c_str(), acl_name, destination_acl->data(),
                           destination_acl->size());
            if (copied != acl_size)
                return false;
        }
    }
#endif

    int descriptor = -1;
    for (int attempt = 0; attempt != 128; ++attempt) {
        temporary = temporary_path(destination, next_serial.fetch_add(1));
        descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (descriptor >= 0)
            break;
        if (errno != EEXIST) {
#if defined(__APPLE__)
            if (destination_acl != nullptr)
                ::acl_free(destination_acl);
#endif
            return false;
        }
    }
    if (descriptor < 0) {
#if defined(__APPLE__)
        if (destination_acl != nullptr)
            ::acl_free(destination_acl);
#endif
        return false;
    }

    bool complete = true;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto chunk = std::min<std::size_t>(
            text.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto written = ::write(descriptor, text.data() + offset, chunk);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            complete = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (complete && destination_exists) {
        struct stat temporary_status{};
        complete = ::fstat(descriptor, &temporary_status) == 0;
        if (complete && (temporary_status.st_uid != destination_status.st_uid ||
                         temporary_status.st_gid != destination_status.st_gid))
            complete =
                ::fchown(descriptor, destination_status.st_uid, destination_status.st_gid) == 0;
        complete = complete && ::fchmod(descriptor, destination_status.st_mode &
                                                        static_cast<mode_t>(07777)) == 0;
#if defined(__APPLE__)
        if (complete && destination_acl != nullptr) {
            complete = ::acl_set_fd_np(descriptor, destination_acl, ACL_TYPE_EXTENDED) == 0;
        } else if (complete)
            complete = clear_extended_acl(descriptor);
#elif defined(__linux__)
        constexpr auto acl_name = "system.posix_acl_access";
        if (complete && destination_acl) {
            complete = ::fsetxattr(descriptor, acl_name, destination_acl->data(),
                                   destination_acl->size(), 0) == 0;
        } else if (complete && ::fremovexattr(descriptor, acl_name) != 0 &&
                   errno != ENODATA) {
            complete = false;
        }
#endif
    }
#if defined(__APPLE__)
    if (destination_acl != nullptr)
        ::acl_free(destination_acl);
#endif
    complete = complete && sync_file(descriptor);
    complete = ::close(descriptor) == 0 && complete;
    if (!complete) {
        remove_temporary(temporary);
        return false;
    }
    if (::rename(temporary.c_str(), destination.c_str()) == 0)
        return sync_parent_directory(destination);
#endif
    remove_temporary(temporary);
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
        const auto maximum_command_bytes = pulp::timeline::DecodeLimits{}.max_input_bytes;
        const auto commands = read_text_bounded(args[2], maximum_command_bytes);
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
            output = args[4];
        }
        auto result = pulp::tools::timeline::command_apply(args[1], *commands.text);
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
