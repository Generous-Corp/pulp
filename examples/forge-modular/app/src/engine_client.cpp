// The engine, reached as a subprocess.
//
// Generation is Python: generate.py compiles a module, patch.py builds and
// gates a patch. Running them out-of-process is not a shortcut -- it is what
// keeps a compiler out of a host's plugin sandbox, and what stops the same
// pipeline existing in two places once the plugin ships beside the app.
//
// Every run is detached and its output goes to a log. A generation takes a
// minute or more; blocking a UI thread on it, let alone an audio thread,
// would be indefensible.

#include "forge_modular/shell.hpp"

#include <pulp/runtime/log.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <string>

namespace forge_modular {
namespace {

/// Single-quote a string for a shell, closing and reopening around any quote
/// it contains. A prompt is user text and will eventually contain one.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

}  // namespace

/// Runs the generator scripts from the repository they live in.
class SubprocessEngine : public EngineClient {
public:
    explicit SubprocessEngine(std::filesystem::path tools_dir)
        : tools_(std::move(tools_dir)) {}

    bool available() const override {
        return std::filesystem::exists(tools_ / "generate.py")
            && std::filesystem::exists(tools_ / "patch.py");
    }

    bool ensure_running() override {
        // Nothing to start: each generation is its own process. The check is
        // whether the scripts are where we expect, which is a real failure
        // worth reporting rather than a transient one worth retrying.
        if (available()) return true;
        pulp::runtime::log_error(
            "Forge Modular: the generator scripts are not at {} — set "
            "FORGE_MODULAR_TOOLS to the tools/rack directory", tools_.string());
        return false;
    }

    std::string last_error() const override { return last_error_; }

    void submit(const std::string& prompt, bool patch_mode) override {
        const std::filesystem::path script =
            tools_ / (patch_mode ? "patch.py" : "generate.py");
        const std::string log = (log_dir() / "last-run.log").string();

        std::string cmd = "cd " + shell_quote(tools_.string()) + " && ";
        cmd += "python3 " + shell_quote(script.string()) + " ";
        if (patch_mode) cmd += "build ";
        cmd += shell_quote(prompt);

        if (!std::filesystem::exists(script)) {
            // The failure that shipped: no generator, and nothing said so.
            last_error_ = "the generator is not installed (" + script.string() + ")";
            pulp::runtime::log_info("Forge Modular: cannot build — {}", last_error_);
            return;
        }

        // A header written before the spawn, so an empty log is impossible and
        // "nothing happened" can always be told apart from "it started".
        {
            std::ofstream head(log, std::ios::trunc);
            head << "forge-modular: " << (patch_mode ? "patch" : "module")
                 << "\ntools: " << tools_.string()
                 << "\nprompt: " << prompt << "\n---\n";
        }
        cmd += " >> " + shell_quote(log) + " 2>&1 &";

        pulp::runtime::log_info("Forge Modular: building a {} — output to {}",
                                patch_mode ? "patch" : "module", log);
        last_error_.clear();
        if (std::system(cmd.c_str()) != 0)
            last_error_ = "the generator could not be started";
    }

    std::filesystem::path log_dir() const {
        auto d = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".")
                 / "Library" / "Application Support" / "Forge Modular";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }

private:
    std::filesystem::path tools_;
    std::string last_error_;
};

/// The tools directory that actually exists on this machine.
///
/// FORGE_MODULAR_TOOLS_DIR is an absolute path into the source tree. On the
/// machine that built the app that is right and convenient; anywhere else it
/// does not exist, so `available()` was false, `ensure_running()` refused, and
/// Build did nothing whatsoever -- no log, no message, no clue. The bundled copy
/// is what makes an installed app able to generate at all.
std::filesystem::path resolve_tools_dir() {
    std::error_code ec;
    if (const char* env = std::getenv("FORGE_MODULAR_TOOLS")) return env;

    const std::filesystem::path source{FORGE_MODULAR_TOOLS_DIR};
    if (std::filesystem::exists(source / "generate.py", ec)) return source;

#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (size > 0 && _NSGetExecutablePath(buf.data(), &size) == 0) {
        const std::filesystem::path exe(buf.c_str());
        const auto bundled =
            exe.parent_path().parent_path() / "Resources" / "tools";
        if (std::filesystem::exists(bundled / "generate.py", ec)) return bundled;
    }
#endif
    return source;   // report the path we wanted, so the failure names it
}

std::unique_ptr<EngineClient> make_engine() {
    return std::make_unique<SubprocessEngine>(resolve_tools_dir());
}

}  // namespace forge_modular
