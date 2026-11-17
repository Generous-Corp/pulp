// SPDX-License-Identifier: MIT
#include "windows_callback_traps.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <mutex>

namespace pulp::test::windows_traps {
namespace {
thread_local Counts* g_counts = nullptr;

void note(Kind kind) noexcept {
    if (g_counts != nullptr)
        ++g_counts->calls[static_cast<std::size_t>(kind)];
}

// One trap per entry point: count, then forward to the original target.
#define PULP_TRAP(result, name, kind, parameters, arguments)                                       \
    using name##_type = result(WINAPI*) parameters;                                                \
    name##_type real_##name = nullptr;                                                             \
    result WINAPI trap_##name parameters {                                                         \
        note(Kind::kind);                                                                          \
        return real_##name arguments;                                                              \
    }

PULP_TRAP(LPVOID, HeapAlloc, allocation, (HANDLE heap, DWORD flags, SIZE_T bytes),
          (heap, flags, bytes))
PULP_TRAP(LPVOID, HeapReAlloc, allocation, (HANDLE heap, DWORD flags, LPVOID block, SIZE_T bytes),
          (heap, flags, block, bytes))
PULP_TRAP(BOOL, HeapFree, allocation, (HANDLE heap, DWORD flags, LPVOID block),
          (heap, flags, block))
PULP_TRAP(LPVOID, VirtualAlloc, allocation, (LPVOID at, SIZE_T bytes, DWORD type, DWORD protect),
          (at, bytes, type, protect))
PULP_TRAP(void, AcquireSRWLockExclusive, lock, (PSRWLOCK value), (value))
PULP_TRAP(void, AcquireSRWLockShared, lock, (PSRWLOCK value), (value))
PULP_TRAP(BOOLEAN, TryAcquireSRWLockExclusive, lock, (PSRWLOCK value), (value))
PULP_TRAP(void, EnterCriticalSection, lock, (LPCRITICAL_SECTION value), (value))
PULP_TRAP(DWORD, WaitForSingleObject, wait, (HANDLE handle, DWORD ms), (handle, ms))
PULP_TRAP(DWORD, WaitForSingleObjectEx, wait, (HANDLE handle, DWORD ms, BOOL alertable),
          (handle, ms, alertable))
PULP_TRAP(DWORD, WaitForMultipleObjects, wait,
          (DWORD count, const HANDLE* handles, BOOL all, DWORD ms), (count, handles, all, ms))
PULP_TRAP(DWORD, WaitForMultipleObjectsEx, wait,
          (DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL alertable),
          (count, handles, all, ms, alertable))
PULP_TRAP(void, Sleep, wait, (DWORD ms), (ms))
PULP_TRAP(DWORD, SleepEx, wait, (DWORD ms, BOOL alertable), (ms, alertable))
PULP_TRAP(BOOL, SwitchToThread, wait, (), ())
PULP_TRAP(BOOL, SleepConditionVariableSRW, wait,
          (PCONDITION_VARIABLE variable, PSRWLOCK value, DWORD ms, ULONG flags),
          (variable, value, ms, flags))
PULP_TRAP(BOOL, SleepConditionVariableCS, wait,
          (PCONDITION_VARIABLE variable, PCRITICAL_SECTION value, DWORD ms), (variable, value, ms))
PULP_TRAP(BOOL, WaitOnAddress, wait,
          (volatile VOID * address, PVOID compare, SIZE_T size, DWORD ms),
          (address, compare, size, ms))
PULP_TRAP(BOOL, ReadFile, io,
          (HANDLE file, LPVOID data, DWORD size, LPDWORD moved, LPOVERLAPPED operation),
          (file, data, size, moved, operation))
PULP_TRAP(BOOL, WriteFile, io,
          (HANDLE file, LPCVOID data, DWORD size, LPDWORD moved, LPOVERLAPPED operation),
          (file, data, size, moved, operation))
PULP_TRAP(BOOL, PeekNamedPipe, io,
          (HANDLE pipe, LPVOID data, DWORD size, LPDWORD read, LPDWORD available, LPDWORD left),
          (pipe, data, size, read, available, left))
PULP_TRAP(BOOL, GetOverlappedResult, io,
          (HANDLE file, LPOVERLAPPED operation, LPDWORD moved, BOOL wait),
          (file, operation, moved, wait))
PULP_TRAP(BOOL, CancelIoEx, io, (HANDLE file, LPOVERLAPPED operation), (file, operation))
PULP_TRAP(HANDLE, CreateEventW, handle,
          (LPSECURITY_ATTRIBUTES security, BOOL manual, BOOL initial, LPCWSTR name),
          (security, manual, initial, name))
PULP_TRAP(BOOL, CloseHandle, handle, (HANDLE handle), (handle))
PULP_TRAP(BOOL, DuplicateHandle, handle,
          (HANDLE source_process, HANDLE source, HANDLE target_process, LPHANDLE target,
           DWORD access, BOOL inherit, DWORD options),
          (source_process, source, target_process, target, access, inherit, options))
PULP_TRAP(HANDLE, OpenProcess, process, (DWORD access, BOOL inherit, DWORD id),
          (access, inherit, id))
PULP_TRAP(BOOL, TerminateProcess, process, (HANDLE handle, UINT code), (handle, code))
PULP_TRAP(BOOL, TerminateJobObject, process, (HANDLE handle, UINT code), (handle, code))
#undef PULP_TRAP

struct Entry {
    const char* name;
    void* trap;
    void** real;
};
#define PULP_ENTRY(name)                                                                           \
    Entry {                                                                                        \
        #name, reinterpret_cast<void*>(&trap_##name), reinterpret_cast<void**>(&real_##name)       \
    }
const Entry kEntries[] = {
    PULP_ENTRY(HeapAlloc),
    PULP_ENTRY(HeapReAlloc),
    PULP_ENTRY(HeapFree),
    PULP_ENTRY(VirtualAlloc),
    PULP_ENTRY(AcquireSRWLockExclusive),
    PULP_ENTRY(AcquireSRWLockShared),
    PULP_ENTRY(TryAcquireSRWLockExclusive),
    PULP_ENTRY(EnterCriticalSection),
    PULP_ENTRY(WaitForSingleObject),
    PULP_ENTRY(WaitForSingleObjectEx),
    PULP_ENTRY(WaitForMultipleObjects),
    PULP_ENTRY(WaitForMultipleObjectsEx),
    PULP_ENTRY(Sleep),
    PULP_ENTRY(SleepEx),
    PULP_ENTRY(SwitchToThread),
    PULP_ENTRY(SleepConditionVariableSRW),
    PULP_ENTRY(SleepConditionVariableCS),
    PULP_ENTRY(WaitOnAddress),
    PULP_ENTRY(ReadFile),
    PULP_ENTRY(WriteFile),
    PULP_ENTRY(PeekNamedPipe),
    PULP_ENTRY(GetOverlappedResult),
    PULP_ENTRY(CancelIoEx),
    PULP_ENTRY(CreateEventW),
    PULP_ENTRY(CloseHandle),
    PULP_ENTRY(DuplicateHandle),
    PULP_ENTRY(OpenProcess),
    PULP_ENTRY(TerminateProcess),
    PULP_ENTRY(TerminateJobObject),
};
#undef PULP_ENTRY

std::vector<std::string> redirect() {
    std::vector<std::string> redirected;
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0)
        return redirected;
    for (auto* module =
             reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         module->Name != 0; ++module) {
        if (module->OriginalFirstThunk == 0)
            continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + module->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + module->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const auto* import =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            for (const auto& entry : kEntries) {
                if (std::strcmp(reinterpret_cast<const char*>(import->Name), entry.name) != 0)
                    continue;
                DWORD protection = 0;
                if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE,
                                    &protection))
                    break;
                if (*entry.real == nullptr)
                    *entry.real = reinterpret_cast<void*>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<ULONG_PTR>(entry.trap);
                VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), protection,
                               &protection);
                redirected.emplace_back(entry.name);
                break;
            }
        }
    }
    return redirected;
}
} // namespace

const std::vector<std::string>& install() {
    static const std::vector<std::string> redirected = redirect();
    return redirected;
}

Scope::Scope() noexcept : previous_(g_counts) {
    g_counts = &counts_;
}
Scope::~Scope() {
    g_counts = previous_;
}
Counts Scope::counts() const noexcept {
    return counts_;
}

} // namespace pulp::test::windows_traps
