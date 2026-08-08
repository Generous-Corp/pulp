#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/inspect/control_host_preflight.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace std::chrono_literals;

namespace {

bool contains_authority_material(std::string_view value) {
    return value.find("admission-1") != std::string_view::npos ||
           value.find("registration-1") != std::string_view::npos;
}

bool send_wrong_nonce(pulp::inspect::ControlHostBootstrapHandle handle) {
    pulp::events::InterprocessConnection connection;
    connection.set_on_message([&connection](const void*, std::size_t) {
        const auto message = pulp::inspect::encode_control_envelope(
            {.payload = pulp::inspect::ControlHostPreflightResponseEnvelope{std::string(64, 'b')}});
        (void)connection.send_message(message);
    });
    if (!connection.attach_inherited_local_socket(handle))
        return false;
#ifndef _WIN32
    ::close(handle);
#endif
    std::this_thread::sleep_for(500ms);
    return true;
}

bool send_malformed(pulp::inspect::ControlHostBootstrapHandle handle) {
    pulp::events::InterprocessConnection connection;
    connection.set_on_message([&connection](const void*, std::size_t) {
        (void)connection.send_message("not a control envelope");
    });
    if (!connection.attach_inherited_local_socket(handle))
        return false;
#ifndef _WIN32
    ::close(handle);
#endif
    std::this_thread::sleep_for(500ms);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 0; index < argc; ++index)
        if (contains_authority_material(argv[index]))
            return 71;
#ifdef _WIN32
    auto environment = _environ;
#else
    auto environment = environ;
#endif
    for (; environment && *environment; ++environment)
        if (contains_authority_material(*environment))
            return 72;

    const std::string_view mode = argc > 1 ? argv[1] : "--normal";
    if (mode == "--exit")
        return 0;
    if (mode == "--stall") {
        std::this_thread::sleep_for(10s);
        return 0;
    }
    if (mode == "--verbose") {
        const std::string output(256u * 1024u, 'v');
        std::cout << output << std::flush;
    }
    if (mode == "--delayed")
        std::this_thread::sleep_for(200ms);

    const auto handle = pulp::inspect::inherited_control_host_bootstrap_handle();
#ifdef _WIN32
    if (handle == nullptr)
#else
    if (handle < 0)
#endif
        return 65;

    if (mode == "--wrong-nonce")
        return send_wrong_nonce(handle) ? 0 : 66;
    if (mode == "--malformed")
        return send_malformed(handle) ? 0 : 67;
    if (mode != "--normal" && mode != "--unrelated-handle" && mode != "--verbose" &&
        mode != "--delayed")
        return 64;

#ifndef _WIN32
    if (mode == "--unrelated-handle") {
        if (argc != 3)
            return 68;
        const auto unrelated = std::atoi(argv[2]);
        if (::fcntl(unrelated, F_GETFD) >= 0 || errno != EBADF)
            return 69;
    }
#endif

    pulp::inspect::ControlHostPreflightDiagnostics diagnostics;
    auto record = pulp::inspect::receive_control_host_preflight(handle, 2s, std::nullopt,
                                                                &diagnostics);
    if (!record) {
        std::cerr << "preflight status=" << static_cast<unsigned>(diagnostics.status)
                  << " explanation=" << diagnostics.explanation << '\n';
        return 70;
    }
    std::cout << record->registration_id.value << '\n';
    return 0;
}
