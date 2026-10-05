#include "forge/process_engine.hpp"

#include <pulp/platform/child_process.hpp>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace forge_modular {

namespace {

std::atomic<bool> g_generation_launch_claim{false};

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

// Previously shipped Forge catalogues/settings stored GPT-5.6's friendly
// family alias as the model slug. Codex does not expose that alias as a
// runnable model: the CLI needs one of its concrete variants. Resolve legacy
// persisted selections at the provider boundary, while current concrete and
// opaque custom slugs continue through unchanged.
std::string wire_model_for(const forge::ModelSelection& selection) {
    if (selection.provider_id == "codex" && selection.model == "gpt-5.6")
        return "gpt-5.6-sol";
    return selection.model;
}

/// Where a run's prompt is handed over: beside its log, as `<stem>.prompt`.
std::string prompt_path_for(const std::string& log_path) {
    return std::filesystem::path(log_path).replace_extension(".prompt").string();
}

/// Hand the prompt to the generator through a file rather than its argv.
///
/// An argv string has hard limits a prompt does not: Linux refuses any single
/// argument over 128 KiB (MAX_ARG_STRLEN) and macOS refuses argv plus the
/// environment over 1 MiB, so a long prompt failed exec with E2BIG before the
/// generator ever ran. The file is created exclusively and readable only by
/// this user. Whatever sits at the path already is a previous run's leftover
/// (the caller holds the generation claim, so no generator is alive to read
/// it) and is removed, never reused. An embedded NUL is refused: argv could
/// not carry one either, and cutting the prompt there would quietly build
/// from a different request. Returns an error message, or empty on success.
std::string write_prompt_file(const std::string& path, const std::string& prompt) {
    if (prompt.find('\0') != std::string::npos)
        return "the prompt contains a NUL byte, which cannot be handed to the generator";
    ::unlink(path.c_str());
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        return "could not create the prompt file: " + std::string(std::strerror(errno));
    std::size_t written = 0;
    while (written < prompt.size()) {
        const ssize_t n = ::write(fd, prompt.data() + written, prompt.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const std::string why = std::strerror(errno);
            ::close(fd);
            ::unlink(path.c_str());
            return "could not write the prompt file: " + why;
        }
        written += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        const std::string why = std::strerror(errno);
        ::unlink(path.c_str());
        return "could not write the prompt file: " + why;
    }
    return {};
}

/// The `/bin/sh -c` command that runs one generation.
///
/// The prompt is not in it: the tool reads `--prompt-file` and removes the
/// file, so the command's size no longer depends on what the person typed.
std::string generation_command(const std::string& provider, const std::string& wire_model,
                               const std::string& reasoning_effort, const std::string& script,
                               const std::string& verb, const std::string& prompt_path,
                               const std::string& log_path) {
    // -u, because Python BLOCK-BUFFERS stdout when it is not a terminal.
    //
    // The transcript is redirected to a file, so without this nothing reaches
    // the log until 4-8KB has accumulated or the process exits. BuildMonitor
    // tails that file to drive the stage list, so a run that was working
    // perfectly showed an empty log, no stage, and no elapsed time for
    // minutes: observed at 4m50s into a generation with a 0-byte log and a
    // healthy python. Indistinguishable, from the outside, from a run that
    // died on the first line -- which is the reading it invites.
    const std::string bootstrap =
        "import os,sys; os.setsid(); os.execvp(sys.argv[1], sys.argv[1:])";
    std::ostringstream cmd;
    cmd << "exec /usr/bin/env "
        << "-u FORGE_CLAUDE_MODEL -u FORGE_CODEX_MODEL "
        << "-u FORGE_CLAUDE_REASONING_EFFORT "
        << "-u FORGE_CODEX_REASONING_EFFORT "
        << "FORGE_MODEL_PROVIDER=" << shell_quote(provider) << " "
        << (provider == "codex" ? "FORGE_CODEX_MODEL=" : "FORGE_CLAUDE_MODEL=")
        << shell_quote(wire_model) << " "
        << (provider == "codex"
                ? "FORGE_CODEX_REASONING_EFFORT=" + shell_quote(reasoning_effort) + " "
                : "FORGE_CLAUDE_REASONING_EFFORT=" + shell_quote(reasoning_effort) + " ")
        // The bootstrap makes the retained root the leader of a private
        // session/process group before patch.py can spawn the model CLI. Stop
        // can therefore freeze and signal the whole owned group atomically.
        << "python3 -c " << shell_quote(bootstrap)
        << " python3 -u " << shell_quote(script) << verb
        << "--prompt-file " << shell_quote(prompt_path)
        // Truncate: BuildMonitor treats a shrinking file as a new run, so the
        // transcript starts clean rather than replaying the previous build.
        << " > " << shell_quote(log_path) << " 2>&1";
    return cmd.str();
}

}  // namespace

struct ProcessEngine::RunState {
    std::atomic<pid_t> pid{0};
    std::atomic<bool> cancel_requested{false};
    std::atomic<int> launch_state{-1};  // -1 idle, 0 pending, 1 started, 2 failed
    std::atomic<bool> terminal_written{false};
    std::mutex details_mutex;
    std::mutex cancel_mutex;
    std::shared_ptr<pulp::platform::ChildProcess> child;
    std::string expected_script;
    std::string log_path;
    std::string prompt_path;
    std::string provider;
};

void ProcessEngine::append_terminal(const std::shared_ptr<RunState>& state,
                                    const std::string& line) {
    if (state->terminal_written.exchange(true, std::memory_order_acq_rel)) return;
    std::string log;
    {
        std::lock_guard lock(state->details_mutex);
        log = state->log_path;
    }
    if (!log.empty()) {
        std::ofstream f(log, std::ios::app);
        f << "\n" << line << "\n";
    }
}

ProcessEngine::ProcessEngine(std::string tools_dir, std::string log_path)
    : tools_dir_(std::move(tools_dir)), log_path_(std::move(log_path)),
      run_state_(std::make_shared<RunState>()) {
    // `runs/` beside the log the shell starts on. Kept beside it rather than
    // in a temp directory because a generation costs minutes and its
    // transcript is the only record of what the model was asked and answered;
    // /tmp is cleared out from under exactly that.
    if (!log_path_.empty())
        log_base_ = (std::filesystem::path(log_path_).parent_path() / "runs")
                        .string();
}

ProcessEngine::~ProcessEngine() {
    // This engine is owned by the processor/app, not by an editor view. Thus a
    // window may close and reopen without interrupting a build, while normal
    // app/plugin teardown reliably stops the exact process this engine owns.
    run_state_->cancel_requested.store(true, std::memory_order_release);
    for (int i = 0; i < 100 &&
                    run_state_->launch_state.load(std::memory_order_acquire) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (run_state_->launch_state.load(std::memory_order_acquire) == 1)
        cancel_run(run_state_, /*mark_inactive_cancelled=*/false);
}

int ProcessEngine::run_tool(const std::string& executable,
                            const std::vector<std::string>& args,
                            std::string& output,
                            const std::string& working_dir) {
    output.clear();
    pulp::platform::ProcessOptions opts;
    opts.working_directory = working_dir;
    // Both streams, into one string, because every caller here reports the
    // whole transcript to a person: a Python traceback arrives on stderr, and
    // a message that drops it says only "it failed".
    const auto r = pulp::platform::ChildProcess::run(executable, args, opts);
    output = r.stdout_output + r.stderr_output;
    return r.exit_code;
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
        python_ok_.store(
            run_tool("python3", {"-c", "pass"}, out) == 0 ? 1 : 0,
            std::memory_order_relaxed);
    }
    if (python_ok_.load(std::memory_order_relaxed) != 1) {
        error_ = "python3 is not available on this machine";
        return false;
    }
    return true;
}

bool ProcessEngine::generator_running() const {
    // Matched on the SCRIPT plus its verb, not on the interpreter invocation.
    //
    // This used to look for the literal "python3 patch.py", which stopped
    // matching the moment the launch line gained a flag: `python3 -u patch.py`
    // is the same run and a different string, so the guard reported "nothing
    // running" while a generation was in flight, silently. Anything that
    // matches the interpreter's exact spelling breaks on the next flag.
    //
    // Do not put the probe through /bin/sh. A shell receives the whole command
    // as argv text, so `sh -c "pgrep -f 'patch.py build'"` can itself be a
    // match for `patch.py build`. Some shells exec the final command in place
    // and happen to avoid it; correctness cannot depend on that optimization.
    // ChildProcess hands these arguments directly to pgrep instead.
    //
    // The bracketed first character is deliberate too. It is the standard
    // self-excluding pgrep expression: `[p]atch.py` matches `patch.py` in a
    // generator argv, but not the literal `[p]atch.py` pattern in pgrep's own
    // argv. That keeps this correct even without relying on pgrep's usual
    // promise not to report itself.
    std::string out;
    for (const char* pattern : {"[p]atch.py build", "[g]enerate.py "})
        if (run_tool("pgrep", {"-f", pattern}, out) == 0)
            return true;
    return false;
}

bool ProcessEngine::try_claim_generation() {
    bool expected = false;
    if (!g_generation_launch_claim.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return false;
    if (generator_running()) {
        g_generation_launch_claim.store(false, std::memory_order_release);
        return false;
    }
    claimed_by_this_.store(true, std::memory_order_release);
    return true;
}

void ProcessEngine::release_generation_claim() {
    if (claimed_by_this_.exchange(false, std::memory_order_acq_rel))
        g_generation_launch_claim.store(false, std::memory_order_release);
}

void ProcessEngine::submit(const std::string& prompt, bool patch_mode,
                           const forge::ModelSelection& model) {
    error_.clear();
    artifact_.clear();
    if (prompt.find('\0') != std::string::npos) {
        error_ = "the prompt contains a NUL byte, which cannot be handed to the generator";
        release_generation_claim();
        return;
    }

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
        if (ec) {
            error_ = "could not create the generation log directory: " + ec.message();
            release_generation_claim();
            return;
        }
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
                error_ = "could not claim a generation log: " +
                         std::string(std::strerror(errno));
                release_generation_claim();
                return;
            }
        }
    }

    // The same two tools the command line drives. If the app generated by a
    // different route, only one of the two would ever be the tested one.
    const std::string tool = patch_mode ? "patch.py" : "generate.py";
    const std::string verb = patch_mode ? " build " : " ";

    if (model.provider_id != "claude" && model.provider_id != "codex") {
        error_ = "unsupported agent provider '" + model.provider_id + "'";
        release_generation_claim();
        return;
    }
    if (model.model.empty()) {
        error_ = "the selected agent has no model";
        release_generation_claim();
        return;
    }

    const auto script = (std::filesystem::path(tools_dir_) / tool).string();
    {
        std::lock_guard lock(run_state_->details_mutex);
        run_state_->expected_script = script;
        run_state_->log_path = log_path_;
        run_state_->provider = model.provider_id;
    }
    run_state_->pid.store(0, std::memory_order_release);
    run_state_->cancel_requested.store(false, std::memory_order_release);
    run_state_->launch_state.store(0, std::memory_order_release);
    run_state_->terminal_written.store(false, std::memory_order_release);

    const std::string wire_model = wire_model_for(model);
    const std::string reasoning_effort(
        forge::to_string(forge::Settings::instance().effort()));

    // The prompt travels by file. While this engine holds the generation
    // claim no generator is alive, so any hand-off file still beside the logs
    // belongs to a run that never read it (it died first, or was stopped
    // before it got there): remove it rather than leave the text on disk.
    const std::string prompt_path = prompt_path_for(log_path_);
    if (claimed_by_this_.load(std::memory_order_acquire)) {
        std::error_code ec;
        const auto dir = std::filesystem::path(prompt_path).parent_path();
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            if (entry.path().extension() == ".prompt")
                std::filesystem::remove(entry.path(), ec);
    }
    if (const std::string failure = write_prompt_file(prompt_path, prompt);
        !failure.empty()) {
        error_ = failure;
        release_generation_claim();
        return;
    }
    {
        std::lock_guard lock(run_state_->details_mutex);
        run_state_->prompt_path = prompt_path;
    }
    // The processor, not the editor view, owns this non-blocking ChildProcess.
    // It therefore remains alive across editor-window closure, while the
    // engine destructor explicitly cancels its retained PID on app/plugin
    // teardown, so quitting cannot leave an agent running.
    const std::string command = generation_command(model.provider_id, wire_model,
                                                   reasoning_effort, script, verb,
                                                   prompt_path, log_path_);
    // On a worker, not the UI thread. Even backgrounded, this forks a shell and
    // touches tools_dir_ -- which may live on a removable, network-backed or
    // TCC-gated volume, where either can block for seconds. That is what made
    // the app appear to freeze on Build, which is precisely when a user is
    // least willing to believe it is still alive.
    const bool releases_claim =
        claimed_by_this_.exchange(false, std::memory_order_acq_rel);
    auto state = run_state_;
    const auto working_dir = tools_dir_;
    std::thread([command, prompt_path, working_dir, releases_claim, state]() {
        auto child = std::make_shared<pulp::platform::ChildProcess>();
        pulp::platform::ProcessOptions opts;
        opts.working_directory = working_dir;
        opts.capture_stdout = false;
        opts.capture_stderr = false;
        const bool started = child->start("/bin/sh", {"-c", command}, opts);
        if (started) {
            {
                std::lock_guard lock(state->details_mutex);
                state->child = child;
            }
            state->pid.store(static_cast<pid_t>(child->process_id()),
                             std::memory_order_release);
            state->launch_state.store(1, std::memory_order_release);
        } else {
            ::unlink(prompt_path.c_str());  // nothing will ever read it
            state->launch_state.store(2, std::memory_order_release);
            std::string provider;
            {
                std::lock_guard lock(state->details_mutex);
                provider = state->provider;
            }
            append_terminal(state, "model call failed: could not launch the selected " +
                                       provider + " agent");
        }
        if (releases_claim)
            g_generation_launch_claim.store(false, std::memory_order_release);
        if (state->cancel_requested.load(std::memory_order_acquire) && started)
            ProcessEngine::cancel_run(state, true);  // Stop during launch is not lost.
    }).detach();
}

void ProcessEngine::cancel_generation() {
    run_state_->cancel_requested.store(true, std::memory_order_release);
    // submit() deliberately does the launch off the UI thread. Stop or app
    // quit can therefore arrive after the user action but before the worker
    // has published `$!`. Wait only for that bounded handoff; the worker also
    // observes cancel_requested, so a slow or failed launch cannot escape.
    for (int i = 0; i < 100 &&
                    run_state_->pid.load(std::memory_order_acquire) <= 1 &&
                    run_state_->launch_state.load(std::memory_order_acquire) == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (run_state_->launch_state.load(std::memory_order_acquire) == 2) return;
    cancel_run(run_state_, /*mark_inactive_cancelled=*/true);
}

void ProcessEngine::cancel_run(const std::shared_ptr<RunState>& state,
                               bool mark_inactive_cancelled) {
    std::lock_guard cancelling(state->cancel_mutex);
    const pid_t pid = state->pid.load(std::memory_order_acquire);
    if (pid <= 1) {
        if (mark_inactive_cancelled)
            append_terminal(state, "generation cancelled by user");
        return;
    }

    std::string expected;
    std::shared_ptr<pulp::platform::ChildProcess> child;
    {
        std::lock_guard lock(state->details_mutex);
        expected = state->expected_script;
        child = state->child;
    }
    if (!child || child->process_id() != pid || !child->is_running()) {
        state->pid.store(0, std::memory_order_release);
        if (mark_inactive_cancelled)
            append_terminal(state, "generation cancelled by user");
        return;
    }
    std::string command;
    if (run_tool("ps", {"-p", std::to_string(pid), "-o", "command="}, command) != 0 ||
        expected.empty() || command.find(expected) == std::string::npos) {
        // The child already ended or the PID was reused. Never turn a stale
        // integer into permission to kill an unrelated process.
        state->pid.store(0, std::memory_order_release);
        append_terminal(state,
                        "generation stop failed: could not safely identify the owned run");
        return;
    }

    // patch.py became the leader of a private process group before it could
    // spawn a model CLI. Never use a negative PID unless that invariant is
    // live now: signalling the app's own process group would be catastrophic.
    pid_t pgid = ::getpgid(pid);
    for (int i = 0; i < 100 && pgid != pid && child->is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pgid = ::getpgid(pid);
    }
    if (pgid != pid) {
        state->pid.store(0, std::memory_order_release);
        append_terminal(state,
                        "generation stop failed: owned process group was not established");
        return;
    }
    ::kill(-pgid, SIGSTOP);
    ::kill(-pgid, SIGTERM);
    ::kill(-pgid, SIGCONT);  // deliver TERM after the group-wide freeze
    for (int i = 0; i < 20; ++i) {
        if (::kill(pid, 0) != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (::kill(pid, 0) == 0) ::kill(-pgid, SIGKILL);
    state->pid.store(0, std::memory_order_release);
    // A run stopped before its tool read the prompt leaves the file behind.
    std::string prompt_path;
    {
        std::lock_guard lock(state->details_mutex);
        prompt_path = state->prompt_path;
    }
    if (!prompt_path.empty()) ::unlink(prompt_path.c_str());

    append_terminal(state, "generation cancelled by user");
}

std::string ProcessEngine::explain(const std::string& patch_path) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(patch_path, ec))
        return "There is no patch open to explain yet.";

    // No shell: the patch path is an argument, and `working_directory` is what
    // the `cd` was for. A path with a space or a quote in it used to depend on
    // shell_quote getting it exactly right.
    std::string out;
    if (run_tool("python3", {"patch.py", "explain", patch_path}, out,
                 tools_dir_) != 0 || out.empty())
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
