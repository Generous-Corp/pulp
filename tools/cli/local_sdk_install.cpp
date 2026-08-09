// local_sdk_install.cpp — forge-dev SDK build, validation, and atomic publish.

#include "local_sdk_install.hpp"

#include "cli_common.hpp"
#include "local_sdk_profile.hpp"
#include "shell_quote.hpp"
#include "tartci_lease.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace local_sdk = pulp::cli::local_sdk;

std::string capture_command_text(const std::string& command) {
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe)
        return {};
    std::string output;
    char buffer[4096];
    while (size_t count = fread(buffer, 1, sizeof(buffer), pipe))
        output.append(buffer, count);
#if defined(_WIN32)
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    if (rc != 0)
        return {};
    return trim(output);
}

std::string git_output(const fs::path& checkout, const std::string& args) {
    return capture_command_text("git -C " + shell_quote(checkout) + " " + args + " 2>/dev/null");
}

bool git_checkout_clean(const fs::path& checkout, bool& query_ok) {
    constexpr const char* marker = "__PULP_GIT_STATUS_OK__";
    auto output = capture_command_text("git -C " + shell_quote(checkout) +
                                       " status --porcelain --untracked-files=all 2>/dev/null"
                                       " && printf '\\n" +
                                       marker + "\\n'");
    const auto marker_pos = output.rfind(marker);
    query_ok = marker_pos != std::string::npos && trim(output.substr(marker_pos)) == marker;
    if (!query_ok)
        return false;
    return trim(output.substr(0, marker_pos)).empty();
}

std::string first_line(std::string value) {
    const auto newline = value.find('\n');
    if (newline != std::string::npos)
        value.resize(newline);
    return trim(value);
}

std::string read_trimmed_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    std::string value{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return trim(value);
}

std::string macos_floor_from_manifest(const fs::path& repo_root) {
    const auto text = read_trimmed_file(repo_root / "tools" / "deps" / "min_os.json");
    const auto platform = text.find("\"macos-arm64\"");
    if (platform == std::string::npos)
        return {};
    const auto floor = text.find("\"floor\"", platform);
    const auto colon = text.find(':', floor);
    const auto quote = text.find('"', colon);
    const auto end = text.find('"', quote + 1);
    if (floor == std::string::npos || colon == std::string::npos || quote == std::string::npos ||
        end == std::string::npos)
        return {};
    return text.substr(quote + 1, end - quote - 1);
}

std::string dependency_git_sha(const fs::path& repo_root, const char* relative) {
    auto path = repo_root / relative;
    if (!fs::exists(path))
        return {};
    bool query_ok = false;
    if (!git_checkout_clean(path, query_ok) || !query_ok)
        return {};
    return git_output(path, "rev-parse HEAD");
}

class ScopedGitSnapshot {
public:
    ScopedGitSnapshot(fs::path source_checkout, fs::path snapshot_path)
        : source_checkout_(std::move(source_checkout)), snapshot_path_(std::move(snapshot_path)) {}

    bool create(const std::string& source_git_sha, std::string& error) {
        std::error_code ec;
        fs::create_directories(snapshot_path_.parent_path(), ec);
        if (ec) {
            error = "could not create SDK source-snapshot parent: " + ec.message();
            return false;
        }
        fs::remove_all(snapshot_path_, ec);
        const auto command =
            "git clone --shared --no-checkout --quiet " + shell_quote(source_checkout_) + " " +
            shell_quote(snapshot_path_) + " && git -C " + shell_quote(snapshot_path_) +
            " checkout --detach --quiet " + shell_quote(source_git_sha);
        if (run_with_spinner(command, "Creating immutable Pulp source snapshot") != 0) {
            fs::remove_all(snapshot_path_, ec);
            error = "could not create detached source snapshot clone at " + snapshot_path_.string();
            return false;
        }
        active_ = true;
        return true;
    }

    ~ScopedGitSnapshot() {
        if (!active_)
            return;
        std::error_code ec;
        fs::remove_all(snapshot_path_, ec);
        if (ec)
            std::cerr << "Warning: could not remove temporary SDK source snapshot "
                      << snapshot_path_.string() << ": " << ec.message() << "\n";
    }

    const fs::path& path() const { return snapshot_path_; }

    ScopedGitSnapshot(const ScopedGitSnapshot&) = delete;
    ScopedGitSnapshot& operator=(const ScopedGitSnapshot&) = delete;

private:
    fs::path source_checkout_;
    fs::path snapshot_path_;
    bool active_ = false;
};

local_sdk::Identity collect_identity(const fs::path& repo_root, const std::string& version) {
    local_sdk::Identity identity;
    identity.sdk_version = version;
    identity.source_git_sha = git_output(repo_root, "rev-parse HEAD");
    identity.platform = detect_platform();
    identity.cmake_version = first_line(capture_command_text("cmake --version 2>/dev/null"));
    identity.generator = capture_command_text(
        "command -v ninja >/dev/null 2>&1 && printf Ninja || printf 'Unix Makefiles'");
    identity.compiler = first_line(capture_command_text("/usr/bin/clang++ --version 2>/dev/null"));
    identity.macos_sdk = capture_command_text("xcrun --sdk macosx --show-sdk-version 2>/dev/null");
    identity.deployment_target = macos_floor_from_manifest(repo_root);
    identity.skia_identity =
        read_trimmed_file(repo_root / "external" / "skia-build" / ".skia-asset-sha256");
    identity.vst3_git_sha = dependency_git_sha(repo_root, "external/vst3sdk");
    identity.ausdk_git_sha = dependency_git_sha(repo_root, "external/AudioUnitSDK");
    return identity;
}

bool identity_complete(const local_sdk::Identity& identity, std::string& error) {
    const std::pair<const char*, const std::string*> required[] = {
        {"source Git SHA", &identity.source_git_sha},
        {"platform", &identity.platform},
        {"CMake version", &identity.cmake_version},
        {"CMake generator", &identity.generator},
        {"Apple compiler", &identity.compiler},
        {"macOS SDK", &identity.macos_sdk},
        {"macOS deployment target", &identity.deployment_target},
        {"Skia identity", &identity.skia_identity},
        {"VST3 SDK Git SHA", &identity.vst3_git_sha},
        {"AudioUnitSDK Git SHA", &identity.ausdk_git_sha},
    };
    for (const auto& [name, value] : required) {
        if (value->empty()) {
            error = std::string("could not resolve ") + name;
            return false;
        }
    }
    if (identity.platform != "darwin-arm64") {
        error = "forge-dev is supported only by the Apple Silicon CLI";
        return false;
    }
    return true;
}

std::string command_from_args(const std::vector<std::string>& args) {
    std::string command;
    for (const auto& arg : args) {
        if (!command.empty())
            command += " ";
        command += shell_quote(arg);
    }
    return command;
}

void print_validation_errors(const local_sdk::Validation& validation, const char* heading) {
    std::cerr << "Error: " << heading << ":\n";
    for (const auto& error : validation.errors)
        std::cerr << "  - " << error << "\n";
}

bool normalize_installed_archives_for_arm64(const fs::path& prefix, std::string& error) {
    std::size_t checked = 0;
    for (const auto& entry : fs::recursive_directory_iterator(prefix)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".a")
            continue;
        ++checked;
        const auto archs =
            capture_command_text("xcrun lipo -archs " + shell_quote(entry.path()) + " 2>/dev/null");
        switch (local_sdk::archive_slice_action(archs)) {
            case local_sdk::ArchiveSliceAction::Keep:
                break;
            case local_sdk::ArchiveSliceAction::Reject:
                error = entry.path().filename().string() + " has architectures `" +
                        (archs.empty() ? "unknown" : archs) + "` with no arm64 slice";
                return false;
            case local_sdk::ArchiveSliceAction::ThinToArm64: {
                auto thin = entry.path();
                thin += ".pulp-arm64-thin";
                std::error_code ec;
                fs::remove(thin, ec);
                const auto command = "xcrun lipo " + shell_quote(entry.path()) +
                                     " -thin arm64 -output " + shell_quote(thin);
                if (capture_command_text(command + " && printf arm64") != "arm64" ||
                    capture_command_text("xcrun lipo -archs " + shell_quote(thin) +
                                         " 2>/dev/null") != "arm64") {
                    fs::remove(thin, ec);
                    error = "could not thin " + entry.path().filename().string() +
                            " to arm64";
                    return false;
                }
                std::error_code copy_error;
                fs::copy_file(thin, entry.path(), fs::copy_options::overwrite_existing,
                              copy_error);
                fs::remove(thin, ec);
                if (copy_error) {
                    error = "could not publish thinned " + entry.path().filename().string() +
                            ": " + copy_error.message();
                    return false;
                }
                break;
            }
        }
    }
    if (checked == 0) {
        error = "no installed static libraries were found";
        return false;
    }
    return true;
}

void remove_staging(const fs::path& prefix, const fs::path& build) {
    std::error_code ignored;
    fs::remove_all(prefix, ignored);
    fs::remove_all(build, ignored);
}

} // namespace

fs::path ensure_forge_dev_sdk(const fs::path& repo_root) {
#if !defined(__APPLE__) || !(defined(__aarch64__) || defined(__arm64__))
    (void)repo_root;
    std::cerr << "Error: the forge-dev SDK profile requires Apple Silicon macOS.\n";
    return {};
#else
    if (repo_root.empty() || pulp_home().empty()) {
        std::cerr << "Error: forge-dev requires a Pulp checkout and PULP_HOME.\n";
        return {};
    }

    std::error_code ec;
    auto home = fs::absolute(pulp_home(), ec).lexically_normal();
    if (ec || !home.is_absolute()) {
        std::cerr << "Error: forge-dev requires an absolute PULP_HOME: " << ec.message() << "\n";
        return {};
    }

    bool status_ok = false;
    if (!git_checkout_clean(repo_root, status_ok) || !status_ok) {
        std::cerr << "Error: forge-dev requires a completely clean committed checkout "
                     "(including no untracked files).\n";
        return {};
    }

    const auto resolved_sha = git_output(repo_root, "rev-parse HEAD");
    if (resolved_sha.empty()) {
        std::cerr << "Error: could not resolve the checkout HEAD for forge-dev.\n";
        return {};
    }

    const auto unique = std::to_string(getpid());
    const auto snapshot_path =
        home / "sdk-source-dev" / "forge-v1" / resolved_sha / (".snapshot-" + unique);
    ScopedGitSnapshot snapshot(repo_root, snapshot_path);
    std::string snapshot_error;
    if (!snapshot.create(resolved_sha, snapshot_error)) {
        std::cerr << "Error: " << snapshot_error << "\n";
        return {};
    }

    if (ensure_checkout_dependencies(snapshot.path()) != 0)
        return {};
    const auto skia_fetch = snapshot.path() / "tools" / "scripts" / "fetch_skia_for_release.py";
    if (!fs::is_regular_file(skia_fetch) ||
        run_with_spinner("cd " + shell_quote(snapshot.path()) +
                             " && env -u SKIA_DIR -u PULP_USE_BAKED_SKIA python3 " +
                             shell_quote(skia_fetch) + " darwin-arm64",
                         "Preparing pinned arm64 Skia") != 0) {
        std::cerr << "Error: could not prepare the pinned arm64 Skia toolchain.\n";
        return {};
    }

    // Dependency preparation may populate ignored caches, but it must never
    // mutate source content behind the identity we are about to publish.
    if (!git_checkout_clean(snapshot.path(), status_ok) || !status_ok) {
        std::cerr << "Error: immutable source snapshot changed while preparing SDK dependencies.\n";
        return {};
    }

    const auto version = read_project_cmake_version(snapshot.path());
    if (version.empty()) {
        std::cerr << "Error: could not resolve PROJECT_VERSION from the source snapshot.\n";
        return {};
    }
    const auto identity = collect_identity(snapshot.path(), version);
    std::string identity_error;
    if (!identity_complete(identity, identity_error)) {
        std::cerr << "Error: " << identity_error << ".\n";
        return {};
    }
    const auto paths = local_sdk::profile_paths(home, identity);

    if (fs::exists(paths.install_prefix)) {
        const auto existing = local_sdk::validate_published_install(paths.install_prefix, identity);
        if (existing.ok)
            return paths.install_prefix;
        print_validation_errors(existing, "existing forge-dev SDK is invalid");
        std::cerr << "Refusing to overwrite immutable SDK prefix " << paths.install_prefix.string()
                  << "\n";
        return {};
    }

    fs::create_directories(paths.install_prefix.parent_path(), ec);
    if (ec) {
        std::cerr << "Error: could not create forge-dev SDK parent: " << ec.message() << "\n";
        return {};
    }

    const auto staging_prefix =
        paths.install_prefix.parent_path() / (".staging-" + paths.input_fingerprint + "-" + unique);
    const auto active_build_dir = paths.build_dir / (".staging-" + unique);
    fs::remove_all(staging_prefix, ec);
    fs::create_directories(paths.build_dir, ec);
    if (ec) {
        std::cerr << "Error: could not create forge-dev build root: " << ec.message() << "\n";
        return {};
    }

    auto configure_args =
        local_sdk::configure_arguments(snapshot.path(), active_build_dir, staging_prefix, identity);
    configure_args.insert(configure_args.begin() + 1, {"-G", identity.generator});
    if (run_with_spinner(command_from_args(configure_args), "Configuring forge-dev SDK") != 0) {
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }

    auto lease = TartciAgentBuildLease::acquire(
        {.project_root = repo_root, .command_kind = "sdk-forge-dev"});
    if (!lease.ok()) {
        std::cerr << "Error: could not acquire build capacity: " << lease.error() << "\n";
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }
    const int jobs = lease.jobs() > 0 ? lease.jobs() : resolve_local_build_jobs();
    std::string install_cmd = "cmake --build " + shell_quote(active_build_dir) +
                              " --target install --parallel " + std::to_string(jobs);
    install_cmd = apply_agent_build_watchdog(apply_agent_build_qos(install_cmd, lease.qos()), jobs,
                                             lease.active());
    if (run_with_spinner(install_cmd, "Building forge-dev SDK") != 0) {
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }

    const auto post_build_identity = collect_identity(snapshot.path(), version);
    if (!git_checkout_clean(snapshot.path(), status_ok) || !status_ok ||
        !(post_build_identity == identity)) {
        std::cerr << "Error: immutable source/dependency identity changed during the SDK build.\n";
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }

    const auto staged = local_sdk::validate_staged_install(
        staging_prefix, active_build_dir, identity,
        snapshot.path() / "external" / "skia-build");
    if (!staged.ok) {
        print_validation_errors(staged, "staged forge-dev SDK failed validation");
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }
    std::string architecture_error;
    if (!normalize_installed_archives_for_arm64(staging_prefix, architecture_error)) {
        std::cerr << "Error: staged forge-dev SDK could not be normalized to arm64: "
                  << architecture_error
                  << "\n";
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }

    std::string provenance_error;
    if (!local_sdk::write_file_atomically(
            staging_prefix / "sdk-provenance.json",
            local_sdk::serialize_provenance(identity, paths.input_fingerprint), provenance_error)) {
        std::cerr << "Error: " << provenance_error << "\n";
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }
    const auto published = local_sdk::validate_published_install(staging_prefix, identity);
    if (!published.ok) {
        print_validation_errors(published, "forge-dev provenance failed validation");
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }

    fs::rename(staging_prefix, paths.install_prefix, ec);
    if (ec) {
        if (fs::exists(paths.install_prefix) &&
            local_sdk::validate_published_install(paths.install_prefix, identity).ok) {
            remove_staging(staging_prefix, active_build_dir);
            return paths.install_prefix;
        }
        std::cerr << "Error: could not atomically publish forge-dev SDK: " << ec.message() << "\n";
        remove_staging(staging_prefix, active_build_dir);
        return {};
    }
    fs::remove_all(active_build_dir, ec);
    return paths.install_prefix;
#endif
}
