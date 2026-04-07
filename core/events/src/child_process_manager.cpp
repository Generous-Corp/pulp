#include <pulp/events/child_process_manager.hpp>
#include <random>
#include <sstream>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#endif

namespace pulp::events {

// ── ConnectedChildProcess ───────────────────────────────────────────────

ConnectedChildProcess::~ConnectedChildProcess() {
    kill();
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

bool ConnectedChildProcess::launch(std::string_view executable,
                                    const std::vector<std::string>& extra_args) {
    // Generate unique pipe name
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << "/tmp/pulp_ipc_" << std::hex << dist(gen);
    pipe_name_ = ss.str();

    // Build args: executable + pipe_name + extra_args
    std::vector<std::string> args;
    args.push_back("--ipc-pipe");
    args.push_back(pipe_name_);
    args.insert(args.end(), extra_args.begin(), extra_args.end());

    // Create server pipe (blocks until child connects)
    // We need to do this in a thread because create_server blocks
    std::atomic<bool> server_created{false};
    std::thread server_thread([this, &server_created]() {
        connection_.on_text_message = [this](std::string_view msg) {
            if (on_message) on_message(msg);
        };
        server_created.store(true);
        connection_.create_server(pipe_name_, IpcTransport::NamedPipe);
    });

    // Wait for server to start listening
    while (!server_created.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Launch the child process
    pid_ = pulp::runtime::launch_process(executable, args);
    if (pid_ < 0) {
        connection_.disconnect();
        server_thread.join();
        return false;
    }

    running_.store(true);
    server_thread.join();

    // Monitor thread — watches for child exit
    monitor_thread_ = std::thread([this]() {
        while (running_.load()) {
            if (!pulp::runtime::is_process_running(pid_)) {
                running_.store(false);
                if (on_exit) on_exit(0);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    return true;
}

bool ConnectedChildProcess::send_message(std::string_view message) {
    return connection_.send_message(message);
}

void ConnectedChildProcess::kill() {
    if (!running_.load()) return;
    running_.store(false);

#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid_));
    if (h) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
#else
    if (pid_ > 0)
        ::kill(static_cast<pid_t>(pid_), SIGTERM);
#endif

    connection_.disconnect();
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

int ConnectedChildProcess::wait_for_exit(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (running_.load()) {
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_ms) return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

#ifndef _WIN32
    int status;
    waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#else
    return 0;
#endif
}

// ── ChildProcessManager ─────────────────────────────────────────────────

ChildProcessManager::~ChildProcessManager() {
    kill_all();
}

ConnectedChildProcess* ChildProcessManager::launch(
    std::string_view executable, const std::vector<std::string>& args) {
    auto child = std::make_unique<ConnectedChildProcess>();
    child->on_exit = [this, raw = child.get()](int code) {
        if (on_child_exit) on_child_exit(raw, code);
    };

    if (!child->launch(executable, args))
        return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    children_.push_back(std::move(child));
    return children_.back().get();
}

int ChildProcessManager::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (auto& c : children_)
        if (c->is_running()) ++count;
    return count;
}

void ChildProcessManager::kill_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : children_)
        c->kill();
}

void ChildProcessManager::wait_all(int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& c : children_)
        c->wait_for_exit(timeout_ms);
}

void ChildProcessManager::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    children_.erase(
        std::remove_if(children_.begin(), children_.end(),
                      [](auto& c) { return !c->is_running(); }),
        children_.end());
}

}  // namespace pulp::events
