// Windows-only `detect_host_version` impl.
//
// Reads the host executable's VS_FIXEDFILEINFO via GetFileVersionInfoW
// and converts the packed dwFileVersionMS/LS into a `HostVersion`.
// Returns an empty `HostVersion` if the version resource is unavailable
// — adapters treat that as "version-gated quirks off".

#ifdef _WIN32

#include <pulp/format/host_version.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma comment(lib, "version.lib")

#include <vector>

namespace pulp::format {

HostVersion detect_host_version(HostType /*type*/) {
    wchar_t path[MAX_PATH] = {};
    DWORD path_len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (path_len == 0 || path_len >= MAX_PATH) return {};

    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0) return {};

    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(path, handle, size, buffer.data())) return {};

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_len = 0;
    if (!VerQueryValueW(buffer.data(), L"\\",
                        reinterpret_cast<LPVOID*>(&info), &info_len)) {
        return {};
    }
    if (info == nullptr) return {};

    HostVersion v;
    v.major = static_cast<int>(HIWORD(info->dwFileVersionMS));
    v.minor = static_cast<int>(LOWORD(info->dwFileVersionMS));
    v.patch = static_cast<int>(HIWORD(info->dwFileVersionLS));
    return v;
}

}  // namespace pulp::format

#endif // _WIN32
