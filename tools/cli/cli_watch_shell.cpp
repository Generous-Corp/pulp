// Signal-aware shellouts for the `pulp dev` / `pulp loop` watch loop.
#include "cli_watch_shell.hpp"
#include "cli_common.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/runtime/temporary_file.hpp>

namespace {
std::atomic<std::sig_atomic_t> watch_stop_signal{0};
static_assert(decltype(watch_stop_signal)::is_always_lock_free);

void request_watch_stop(int signal) noexcept {
    // A signal may run on a heartbeat thread. A lock-free atomic is signal
    // safe and shares this independent stop value without a cross-thread race.
    watch_stop_signal.store(signal, std::memory_order_relaxed);
}
} // namespace

int requested_watch_stop() noexcept {
    return watch_stop_signal.load(std::memory_order_relaxed);
}

WatchSignalScope::WatchSignalScope() {
    watch_stop_signal.store(0, std::memory_order_relaxed);
    previous_interrupt_ = std::signal(SIGINT, request_watch_stop);
    previous_terminate_ = std::signal(SIGTERM, request_watch_stop);
}

WatchSignalScope::~WatchSignalScope() {
    if (previous_interrupt_ != SIG_ERR)
        std::signal(SIGINT, previous_interrupt_);
    if (previous_terminate_ != SIG_ERR)
        std::signal(SIGTERM, previous_terminate_);
}

bool WatchSignalScope::ok() const {
    return previous_interrupt_ != SIG_ERR && previous_terminate_ != SIG_ERR;
}

// std::system temporarily ignores SIGINT process-wide. Keep watch shellouts
// cancellable without changing the CLI's general-purpose command runner.
int run_watch_command(const std::string& command, const std::string& label) try {
    if (const auto signal = requested_watch_stop())
        return 128 + signal;
    const bool spinner = !label.empty() && is_tty() && !g_no_color;
    if (!label.empty() && !spinner)
        std::cout << label << "...\n" << std::flush;
    pulp::runtime::TemporaryFile output_file(".watch-out"), error_file(".watch-err");
    if (output_file.path().empty() || error_file.path().empty())
        return 127;
    const auto redirected = "(" + command + ") >" + shell_quote(output_file.path_string()) +
                            (spinner ? " 2>&1" : " 2>" + shell_quote(error_file.path_string()));
    pulp::platform::ChildProcess child;
    pulp::platform::ProcessOptions options;
    options.capture_stdout = false;
    options.capture_stderr = false;
#ifdef _WIN32
    const auto shell = pulp::runtime::get_env("COMSPEC").value_or("cmd.exe");
    if (!child.start(shell, {"/C", redirected}, options))
        return 127;
#else
    if (!child.start("/bin/sh", {"-c", redirected}, options))
        return 127;
#endif
    std::ifstream output(output_file.path(), std::ios::binary);
    std::ifstream error(error_file.path(), std::ios::binary);
    if (!output.is_open() || !error.is_open())
        return 127;
    std::streamoff output_position = 0, error_position = 0;
    auto forward = [](std::ifstream& input, std::ostream& destination, std::streamoff& position) {
        // clear() alone does not make libc++'s file buffer retry an earlier
        // EOF. Seek to the consumed byte offset before reading a growing file.
        input.clear();
        input.seekg(position);
        std::array<char, 4096> bytes;
        for (int chunk = 0; chunk < 64; ++chunk) {
            input.read(bytes.data(), bytes.size());
            const auto count = input.gcount();
            if (count) {
                destination.write(bytes.data(), count);
                position += count;
            }
            if (count < static_cast<std::streamsize>(bytes.size()))
                break;
        }
        destination.flush();
        return !input;
    };
    const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    unsigned frame = 0;
    const auto started = std::chrono::steady_clock::now();
    while (child.is_running()) {
        if (requested_watch_stop()) {
            child.cancel();
            break;
        }
        if (spinner) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            std::cout << "\r  " << color::cyan() << frames[frame++ % 10] << color::reset() << " "
                      << label << color::dim() << " (" << seconds << "s)" << color::reset() << "  "
                      << std::flush;
        } else {
            forward(output, std::cout, output_position);
            forward(error, std::cerr, error_position);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    const auto result = child.wait();
    const auto signal = requested_watch_stop();
    const int exit_code = signal ? 128 + signal : result.exit_code;
    if (!spinner) {
        while (!forward(output, std::cout, output_position)) {
        }
        while (!forward(error, std::cerr, error_position)) {
        }
    } else {
        std::cout << "\r\033[K";
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        const auto status =
            label + color::dim() + " (" + std::to_string(seconds) + "s)" + color::reset();
        if (!exit_code)
            print_ok(status);
        else {
            print_fail(status);
            std::vector<std::string> tail;
            std::string line;
            while (std::getline(output, line)) {
                if (tail.size() == 20)
                    tail.erase(tail.begin());
                tail.push_back(std::move(line));
            }
            for (const auto& value : tail)
                std::cout << value << '\n';
        }
    }
    return exit_code;
} catch (const std::exception& error) {
    std::cerr << "pulp watch: command setup failed: " << error.what() << '\n';
    return 127;
}
