#include <pulp/inspect/control_host_bootstrap.hpp>

#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

bool contains_authority_material(std::string_view value) {
    return value.find("admission-1") != std::string_view::npos ||
           value.find("registration-1") != std::string_view::npos ||
           value.find("ZW5yb2xsbWVudC0x+/==") != std::string_view::npos;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--stall") {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }
    if (argc != 3 || std::string_view(argv[1]) != "--unrelated-handle")
        return 64;

    for (int index = 0; index < argc; ++index) {
        if (contains_authority_material(argv[index]))
            return 67;
    }
#ifdef _WIN32
    auto environment = _environ;
#else
    auto environment = environ;
#endif
    for (; environment && *environment; ++environment) {
        if (contains_authority_material(*environment))
            return 68;
    }
    std::cout << std::string(2u * 1024u * 1024u, 'x') << std::flush;

#ifdef _WIN32
    const auto unrelated = reinterpret_cast<HANDLE>(std::strtoull(argv[2], nullptr, 10));
    DWORD flags = 0;
    const bool leaked = GetHandleInformation(unrelated, &flags) != 0;
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_INPUT_HANDLE), GetCurrentProcess(),
                         &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return 69;
#else
    const auto unrelated = std::atoi(argv[2]);
    const bool leaked = ::fcntl(unrelated, F_GETFD) >= 0 || errno != EBADF;
    const auto duplicate = ::dup(STDIN_FILENO);
#endif

    const auto inherited = pulp::inspect::inherited_control_host_bootstrap_handle();
#ifdef _WIN32
    if (pulp::inspect::inherited_control_host_bootstrap_handle() != nullptr)
        return 70;
#else
    if (pulp::inspect::inherited_control_host_bootstrap_handle() >= 0)
        return 70;
#endif
    pulp::inspect::ControlHostBootstrapDiagnostics first_diagnostics;
    auto first = pulp::inspect::read_control_host_bootstrap(
        inherited, std::chrono::seconds(2), std::chrono::system_clock::now(), &first_diagnostics);
    if (!first || leaked)
        return 65;

    pulp::inspect::ControlHostBootstrapDiagnostics second_diagnostics;
    auto second = pulp::inspect::read_control_host_bootstrap(
        duplicate, std::chrono::milliseconds(100), std::chrono::system_clock::now(),
        &second_diagnostics);
    if (second)
        return 66;

    std::cout << (first->enrollment_id.empty() ? "decoded-preissued" : "decoded-enrollment") << '\n'
              << static_cast<unsigned>(second_diagnostics.status) << '\n';
    return 0;
}
