#include "forge/process_engine.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <thread>

namespace forge_modular {

namespace {

/// Quote one argument for /bin/sh. Single quotes with the close-escape-reopen
/// dance, so a prompt containing a quote, a semicolon or a backtick is text
/// rather than shell.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

}  // namespace

ProcessEngine::ProcessEngine(std::string tools_dir, std::string log_path)
    : tools_dir_(std::move(tools_dir)), log_path_(std::move(log_path)) {
    // `runs/` beside the log the shell starts on. Kept beside it rather than
    // in a temp directory because a generation costs minutes and its
    // transcript is the only record of what the model was asked and answered;
    // /tmp is cleared out from under exactly that.
    if (!log_path_.empty())
        log_base_ = (std::filesystem::path(log_path_).parent_path() / "runs")
                        .string();
}

int ProcessEngine::run(const std::string& command, std::string& output) {
    output.clear();
    std::FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) return -1;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += buffer.data();
    const int status = ::pclose(pipe);
    return status == -1 ? -1 : WEXITSTATUS(status);
}

bool ProcessEngine::available() const {
    const int cached = available_.load(std::memory_order_relaxed);
    if (cached >= 0) return cached == 1;
    namespace fs = std::filesystem;
    std::error_code ec;
    const bool ok = fs::exists(fs::path(tools_dir_) / "generate.py", ec) &&
                    fs::exists(fs::path(tools_dir_) / "patch.py", ec);
    available_.store(ok ? 1 : 0, std::memory_order_relaxed);
    return ok;
}

bool ProcessEngine::ensure_running() {
    error_.clear();
    // First call still probes, so a genuinely missing toolchain is reported
    // before anything promises a build. Subsequent calls are free.
    if (!available()) {
        error_ = "the generator is not installed";
        return false;
    }
    // Prove the interpreter runs before promising a build. Discovering a
    // missing python3 minutes in, from a log nobody opened, is how a refusal
    // reads as a hang.
    if (python_ok_.load(std::memory_order_relaxed) < 0) {
        std::string out;
        python_ok_.store(run("python3 -c 'pass' 2>&1", out) == 0 ? 1 : 0,
                         std::memory_order_relaxed);
    }
    if (python_ok_.load(std::memory_order_relaxed) != 1) {
        error_ = "python3 is not available on this machine";
        return false;
    }
    return true;
}

bool ProcessEngine::generator_running() const {
    // Matched on the interpreter invocation, not the bare filename: `pgrep -f
    // patch.py` also matches any shell whose command line merely mentions it,
    // including the one doing the matching.
    std::string out;
    for (const char* pattern : {"python3 patch.py", "python3 generate.py"})
        if (run(std::string("pgrep -f '") + pattern + "' >/dev/null 2>&1", out) == 0)
            return true;
    return false;
}

void ProcessEngine::submit(const std::string& prompt, bool patch_mode) {
    error_.clear();
    artifact_.clear();

    // A log of this run's own, beside the one the last run used. Sharing one
    // file let two overlapping generations overwrite each other — see
    // log_path(). The name carries the clock so the directory reads as a
    // history rather than a pile.
    if (!log_base_.empty()) {
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm tm{};
        ::localtime_r(&now, &tm);
        char stamp[32] = {};
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        std::error_code ec;
        std::filesystem::create_directories(log_base_, ec);
        // The name is CLAIMED, not merely checked.
        //
        // Testing exists() and taking the name is not enough: the file does
        // not exist until the run writes it, which happens later and in
        // another process, so two submits inside the same second both saw
        // nothing there and chose the same path — recreating the collision
        // this whole mechanism exists to remove. O_CREAT|O_EXCL either creates
        // the file or fails, atomically, and it does so against every other
        // process on the machine rather than only against this one.
        std::string stem = stamp;
        for (int n = 1;; ++n) {
            const auto candidate =
                std::filesystem::path(log_base_) /
                (n == 1 ? stem + ".log"
                        : stem + "-" + std::to_string(n) + ".log");
            const int fd = ::open(candidate.c_str(),
                                  O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd >= 0) {
                ::close(fd);
                log_path_ = candidate.string();
                break;
            }
            if (errno != EEXIST) {          // out of room, no permission, …
                log_path_ = candidate.string();   // best effort; say nothing false
                break;
            }
        }
    }

    // The same two tools the command line drives. If the app generated by a
    // different route, only one of the two would ever be the tested one.
    const std::string tool = patch_mode ? "patch.py" : "generate.py";
    const std::string verb = patch_mode ? " build " : " ";

    std::ostringstream cmd;
    // nohup + setsid: a generation takes minutes, and without this it is a
    // child of the app's process group -- quitting the window SIGHUPs it
    // mid-build. Observed: a run died right after "manifest + panel
    // validated", never reaching "compiled", because the app was closed.
    cmd << "cd " << shell_quote(tools_dir_) << " && "
        << "nohup python3 " << tool << verb << shell_quote(prompt)
        // Truncate: BuildMonitor treats a shrinking file as a new run, so the
        // transcript starts clean rather than replaying the previous build.
        << " > " << shell_quote(log_path_) << " 2>&1 &";
    // On a worker, not the UI thread. Even backgrounded, this forks a shell and
    // touches tools_dir_ -- which may live on a removable, network-backed or
    // TCC-gated volume, where either can block for seconds. That is what made
    // the app appear to freeze on Build, which is precisely when a user is
    // least willing to believe it is still alive.
    std::thread([command = cmd.str()]() {
        std::string out;
        run(command, out);
    }).detach();
}

std::string ProcessEngine::explain(const std::string& patch_path) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(patch_path, ec))
        return "There is no patch open to explain yet.";

    std::ostringstream cmd;
    cmd << "cd " << shell_quote(tools_dir_) << " && "
        << "python3 patch.py explain " << shell_quote(patch_path) << " 2>&1";
    std::string out;
    if (run(cmd.str(), out) != 0 || out.empty())
        return "I could not read that patch.";
    return out;
}

void ProcessEngine::explain_async(const std::string& patch_path,
                                  std::function<void(std::string)> done) const {
    if (!done) return;
    // Detached: the answer is advisory and the app must stay responsive while
    // it is computed. Copies everything the worker touches, so the engine
    // outliving or not outliving the thread cannot matter.
    std::thread([tools = tools_dir_, patch_path,
                 done = std::move(done)]() mutable {
        ProcessEngine scratch(tools, {});
        done(scratch.explain(patch_path));
    }).detach();
}

}  // namespace forge_modular
