#include <pulp/events/interprocess_connection.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

int parse_int(std::string_view value, int fallback) {
    try {
        return std::stoi(std::string(value));
    } catch (...) {
        return fallback;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string pipe_name;
    int exit_code = 0;
    int hold_ms = 0;
    bool abrupt_exit = false;
    bool wait_for_exit_message = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--ipc-pipe" && i + 1 < argc) {
            pipe_name = argv[++i];
        } else if (arg == "--exit-code" && i + 1 < argc) {
            exit_code = parse_int(argv[++i], exit_code);
        } else if (arg == "--hold-ms" && i + 1 < argc) {
            hold_ms = parse_int(argv[++i], hold_ms);
        } else if (arg == "--abrupt-exit") {
            abrupt_exit = true;
        } else if (arg == "--wait-for-exit-message") {
            wait_for_exit_message = true;
        }
    }

    if (pipe_name.empty()) return 64;

    std::mutex exit_mutex;
    std::condition_variable exit_cv;
    bool exit_requested = false;

    pulp::events::InterprocessConnection connection;
    if (wait_for_exit_message) {
        connection.on_text_message = [&](std::string_view message) {
            if (message != "exit") return;

            {
                std::lock_guard<std::mutex> lock(exit_mutex);
                exit_requested = true;
            }
            exit_cv.notify_all();
        };
    }

    if (!connection.connect(pipe_name, pulp::events::IpcTransport::NamedPipe))
        return 65;

    if (!connection.send_message("ready"))
        return 66;

    if (wait_for_exit_message) {
        const auto timeout = std::chrono::milliseconds(hold_ms > 0 ? hold_ms : 5000);
        std::unique_lock<std::mutex> lock(exit_mutex);
        if (!exit_cv.wait_for(lock, timeout, [&] { return exit_requested; }))
            return 67;
    } else if (hold_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    if (abrupt_exit)
        std::_Exit(exit_code);

    connection.disconnect();
    return exit_code;
}
