// SPDX-License-Identifier: MIT
#pragma once

#ifdef _WIN32

#include <atomic>
#include <cwchar>
#include <iterator>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace pulp::platform {

// Private duplex channel for a retained child's standard input. Anonymous
// pipes are one-way, so this is a single-instance local named pipe whose only
// client is opened here, before any child exists: a squatter on the name makes
// FILE_FLAG_FIRST_PIPE_INSTANCE fail closed, and no later process can connect.
// Both ends are overlapped so each side can bound every transfer to a deadline.
struct DuplexChannel {
    HANDLE parent = INVALID_HANDLE_VALUE;
    HANDLE child = INVALID_HANDLE_VALUE;

    DuplexChannel() = default;
    DuplexChannel(const DuplexChannel&) = delete;
    DuplexChannel& operator=(const DuplexChannel&) = delete;
    ~DuplexChannel() {
        close_all();
    }

    bool create() {
        static std::atomic<unsigned long long> counter{0};
        LARGE_INTEGER ticks{};
        QueryPerformanceCounter(&ticks);
        wchar_t name[128];
        std::swprintf(name, std::size(name), L"\\\\.\\pipe\\pulp-child-%lu-%llx-%llx",
                      GetCurrentProcessId(), static_cast<unsigned long long>(ticks.QuadPart),
                      counter.fetch_add(1, std::memory_order_relaxed));
        constexpr DWORD buffer_bytes = 64 * 1024;
        parent = CreateNamedPipeW(
            name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
            buffer_bytes, buffer_bytes, 0, nullptr);
        if (parent == INVALID_HANDLE_VALUE)
            return false;
        // The client end is created non-inheritable, so nothing launched from
        // this process inherits it until make_child_inheritable() is called
        // for the one launch that is meant to receive it.
        child = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                            nullptr);
        if (child == INVALID_HANDLE_VALUE) {
            close_all();
            return false;
        }
        // The client above is already connected; any other outcome means the
        // instance is not the private pair this channel promises.
        OVERLAPPED connect{};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const bool connected = connect.hEvent != nullptr && !ConnectNamedPipe(parent, &connect) &&
                               GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected && connect.hEvent != nullptr) {
            CancelIoEx(parent, &connect);
            DWORD ignored = 0;
            GetOverlappedResult(parent, &connect, &ignored, TRUE);
        }
        if (connect.hEvent != nullptr)
            CloseHandle(connect.hEvent);
        if (!connected) {
            close_all();
            return false;
        }
        return true;
    }
    // Marks the client end inheritable. Call it immediately before the
    // CreateProcess that names it in its handle list; close_child() ends the
    // window once that call returns.
    bool make_child_inheritable() noexcept {
        return child != INVALID_HANDLE_VALUE &&
               SetHandleInformation(child, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != FALSE;
    }
    HANDLE release_parent() noexcept {
        return std::exchange(parent, INVALID_HANDLE_VALUE);
    }
    void close_child() noexcept {
        if (child != INVALID_HANDLE_VALUE)
            CloseHandle(std::exchange(child, INVALID_HANDLE_VALUE));
    }
    void close_all() noexcept {
        close_child();
        if (parent != INVALID_HANDLE_VALUE)
            CloseHandle(std::exchange(parent, INVALID_HANDLE_VALUE));
    }
};

}  // namespace pulp::platform

#endif  // _WIN32
