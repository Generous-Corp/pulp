// SPDX-License-Identifier: MIT
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
extern char** environ;
#endif

namespace {

int current_process_id() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

std::string read_standard_input() {
    return {std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
}

bool environment_contains(std::string_view needle) {
#ifdef _WIN32
    auto cursor = _environ;
#else
    auto cursor = environ;
#endif
    for (; cursor && *cursor; ++cursor) {
        if (std::string_view(*cursor).find(needle) != std::string_view::npos)
            return true;
    }
    return false;
}

bool inherited_handle_is_open(std::string_view encoded_handle) {
    const auto value = std::stoull(std::string(encoded_handle));
#ifdef _WIN32
    DWORD flags = 0;
    return GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)),
                                &flags) != FALSE;
#else
    errno = 0;
    return ::fcntl(static_cast<int>(value), F_GETFD) >= 0 || errno != EBADF;
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--exit-immediately")
        return 0;

    if (argc == 2 && std::string_view(argv[1]) == "--stall") {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--pid-bound") {
        const auto pid = current_process_id();
        std::cout << "ready:" << pid << '\n' << std::flush;
        return read_standard_input() == std::to_string(pid) ? 0 : 65;
    }

    if (argc == 3 && std::string_view(argv[1]) == "--pid-bound-no-inherit") {
        if (inherited_handle_is_open(argv[2]))
            return 71;
        const auto pid = current_process_id();
        std::cout << "ready:" << pid << '\n' << std::flush;
        return read_standard_input() == std::to_string(pid) ? 0 : 65;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--private-input") {
        constexpr std::string_view secret = "provider-secret-7f3a";
        for (int index = 0; index < argc; ++index) {
            if (std::string_view(argv[index]).find(secret) != std::string_view::npos)
                return 66;
        }
        if (environment_contains(secret))
            return 67;
        if (std::filesystem::directory_iterator(std::filesystem::current_path()) !=
            std::filesystem::directory_iterator{})
            return 68;

        std::cout << std::string(2u * 1024u * 1024u, 'x') << std::flush;
        if (read_standard_input() != secret)
            return 69;
        if (std::filesystem::directory_iterator(std::filesystem::current_path()) !=
            std::filesystem::directory_iterator{})
            return 70;
        std::cout << "secret-received\n";
        return 0;
    }

    return 64;
}
