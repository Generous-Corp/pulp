// SPDX-License-Identifier: MIT
#pragma once

// Windows transport for the isolated CLAP worker: deadline-bounded transfers on
// the retained child's overlapped duplex standard-input channel, an unnamed
// pagefile-backed section shared by handle duplication, and the hello exchange
// that binds the section to one worker process and generation.

#include "isolated_clap_protocol.hpp"

#if defined(_WIN32)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <new>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace pulp::host::isolation::windows {
using Deadline = std::chrono::steady_clock::time_point;

/// The wait in milliseconds for `left` time remaining: at least 1, and at most
/// INFINITE - 1, so a distant deadline never truncates to a short wait and never
/// becomes an unbounded one.
inline DWORD wait_milliseconds(std::chrono::milliseconds left) noexcept {
    return static_cast<DWORD>(
        std::clamp<std::int64_t>(left.count(), 1, static_cast<std::int64_t>(INFINITE) - 1));
}

/// What `transfer` saw when a wait expired, for tests of its cancellation rule.
struct TransferObservation {
    bool timed_out = false;
    /// The expired operation had completed when its storage was released.
    bool settled = false;
};

/// Moves exactly `size` bytes or fails. Every read and write is overlapped and
/// waits on the one absolute deadline; an expired wait cancels the operation and
/// waits for the cancellation, so the buffer is never written after return.
inline bool transfer(std::intptr_t channel, void* data, std::size_t size, bool sending,
                     Deadline deadline, TransferObservation* observation = nullptr) {
    const auto handle = reinterpret_cast<HANDLE>(channel);
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
        return false;
    auto* bytes = static_cast<std::byte*>(data);
    bool ok = true;
    while (ok && size != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ok = false;
            break;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        OVERLAPPED operation{};
        operation.hEvent = event;
        ResetEvent(event);
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 64 * 1024));
        const BOOL started = sending ? WriteFile(handle, bytes, chunk, nullptr, &operation)
                                     : ReadFile(handle, bytes, chunk, nullptr, &operation);
        if (!started && GetLastError() != ERROR_IO_PENDING) {
            ok = false;
            break;
        }
        DWORD moved = 0;
        if (!started) {
            const auto waited = WaitForSingleObject(event, wait_milliseconds(left));
            if (waited != WAIT_OBJECT_0) {
                CancelIoEx(handle, &operation);
                GetOverlappedResult(handle, &operation, &moved, TRUE);
                if (observation != nullptr) {
                    observation->timed_out = true;
                    observation->settled = HasOverlappedIoCompleted(&operation) != FALSE;
                }
                ok = false;
                break;
            }
        }
        if (!GetOverlappedResult(handle, &operation, &moved, FALSE) || moved == 0) {
            ok = false;
            break;
        }
        bytes += moved;
        size -= moved;
    }
    CloseHandle(event);
    return ok;
}
inline bool send_record(std::intptr_t channel, ControlRecord record, Deadline deadline) {
    return transfer(channel, &record, sizeof(record), true, deadline);
}
inline bool receive_record(std::intptr_t channel, ControlRecord& record, Deadline deadline) {
    return transfer(channel, &record, sizeof(record), false, deadline) && record.magic == kMagic &&
           record.abi == kAbi;
}

/// An unnamed section of exactly kMappingBytes. It has no name to squat or
/// guess: the worker receives it only as a handle duplicated into its process.
class SharedSection {
  public:
    SharedSection() = default;
    SharedSection(const SharedSection&) = delete;
    SharedSection& operator=(const SharedSection&) = delete;
    ~SharedSection() {
        reset();
    }

    /// Parent side: create, map and construct a fresh Mapping for `generation`.
    bool create(std::uint64_t generation) {
        reset();
        section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(kMappingBytes), nullptr);
        if (section_ == nullptr || !map()) {
            reset();
            return false;
        }
        mapping_ = new (view_) Mapping{};
        mapping_->generation = generation;
        return true;
    }

    /// Worker side: adopt a handle value received in the hello record. Mapping
    /// kMappingBytes fails for a smaller section or a handle that is not one.
    bool adopt(std::uint64_t handle_value) {
        reset();
        if (handle_value == 0)
            return false;
        section_ = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle_value));
        if (!map()) {
            reset();
            return false;
        }
        mapping_ = static_cast<Mapping*>(view_);
        return true;
    }

    /// Duplicates the section into `process_id` with map read/write access only.
    /// Returns the handle value valid inside that process, or zero.
    std::uint64_t duplicate_into(int process_id) const {
        if (section_ == nullptr || process_id <= 0)
            return 0;
        const HANDLE target =
            OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(process_id));
        if (target == nullptr)
            return 0;
        HANDLE duplicated = nullptr;
        const BOOL ok = DuplicateHandle(GetCurrentProcess(), section_, target, &duplicated,
                                        FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0);
        CloseHandle(target);
        return ok ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(duplicated)) : 0;
    }

    Mapping* mapping() const noexcept {
        return mapping_;
    }

    void reset() noexcept {
        if (view_ != nullptr)
            UnmapViewOfFile(view_);
        if (section_ != nullptr)
            CloseHandle(section_);
        view_ = nullptr;
        section_ = nullptr;
        mapping_ = nullptr;
    }

  private:
    bool map() {
        view_ = MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kMappingBytes);
        return view_ != nullptr;
    }
    HANDLE section_ = nullptr;
    void* view_ = nullptr;
    Mapping* mapping_ = nullptr;
};

/// Parent half of the hello exchange. `hello` carries the identity fields the
/// caller binds (hashes, build); this fills generation, payload size and the
/// worker-side section handle, then requires the worker to echo all of them
/// together with its own process id.
inline bool parent_hello(std::intptr_t channel, int process_id, const SharedSection& section,
                         ControlRecord hello, Deadline deadline) {
    if (section.mapping() == nullptr)
        return false;
    hello.command = Command::hello;
    hello.result = 0;
    hello.generation = section.mapping()->generation;
    hello.payload_bytes = kMappingBytes;
    hello.volume = 0;
    hello.file_generation = 0;
    hello.process_id = 0;
    hello.file = section.duplicate_into(process_id);
    if (hello.file == 0 || hello.generation == 0)
        return false;
    ControlRecord reply;
    return send_record(channel, hello, deadline) && receive_record(channel, reply, deadline) &&
           hello_reply_matches(hello, reply, static_cast<std::uint64_t>(process_id));
}

/// Worker half: receive hello, adopt the section and verify its header before
/// replying. Returns the received record so the caller can check identity
/// fields (hashes, build) before calling reply_hello().
inline bool worker_receive_hello(std::intptr_t channel, SharedSection& section,
                                 ControlRecord& hello, Deadline deadline) {
    if (!receive_record(channel, hello, deadline) || hello.command != Command::hello ||
        hello.generation == 0 || hello.payload_bytes != kMappingBytes || hello.volume != 0 ||
        hello.file_generation != 0 || !section.adopt(hello.file))
        return false;
    const auto* shared = section.mapping();
    return shared->magic == kMagic && shared->abi == kAbi && shared->generation == hello.generation;
}
inline bool worker_reply_hello(std::intptr_t channel, ControlRecord hello, Deadline deadline) {
    hello.process_id = GetCurrentProcessId();
    hello.result = 1;
    return send_record(channel, hello, deadline);
}

} // namespace pulp::host::isolation::windows
#endif
