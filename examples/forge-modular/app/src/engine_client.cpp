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

    void submit(const std::string& prompt, bool patch_mode) override {
        const std::filesystem::path script =
            tools_ / (patch_mode ? "patch.py" : "generate.py");
        const std::string log = (log_dir() / "last-run.log").string();

        std::string cmd = "cd " + shell_quote(tools_.string()) + " && ";
        cmd += "python3 " + shell_quote(script.string()) + " ";
        if (patch_mode) cmd += "build ";
        cmd += shell_quote(prompt);
        // Detached, with output kept: a generation outlives the click by a
        // minute or more, and its log is the only account of what happened.
        cmd += " > " + shell_quote(log) + " 2>&1 &";

        pulp::runtime::log_info("Forge Modular: building a {} — output to {}",
                                patch_mode ? "patch" : "module", log);
        std::system(cmd.c_str());
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
};

std::unique_ptr<EngineClient> make_engine() {
    // An override first, so a packaged build can point at wherever it put the
    // scripts without recompiling.
    if (const char* env = std::getenv("FORGE_MODULAR_TOOLS"))
        return std::make_unique<SubprocessEngine>(std::filesystem::path(env));
    return std::make_unique<SubprocessEngine>(
        std::filesystem::path(FORGE_MODULAR_TOOLS_DIR));
}

}  // namespace forge_modular
