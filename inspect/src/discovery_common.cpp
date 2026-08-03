#include "discovery_internal.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif
#endif

namespace pulp::inspect::discovery_detail {

std::int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool safe_component(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
           });
}

bool valid_loopback_endpoint(std::string_view endpoint) {
    constexpr std::string_view prefix = "127.0.0.1:";
    if (!endpoint.starts_with(prefix))
        return false;
    endpoint.remove_prefix(prefix.size());
    std::uint32_t port = 0;
    const auto [end, error] = std::from_chars(
        endpoint.data(), endpoint.data() + endpoint.size(), port);
    return error == std::errc{} &&
           end == endpoint.data() + endpoint.size() &&
           port >= 1 && port <= 65535;
}

std::string discovery_file_stem(std::string_view session_id,
                                std::string_view instance_id) {
    return std::to_string(session_id.size()) + "-" +
           std::string(session_id) + "-" + std::string(instance_id);
}

std::optional<std::string> process_start_identity(std::int64_t process_id) {
    if (process_id <= 0)
        return std::nullopt;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(process_id));
    if (!process)
        return std::nullopt;
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(process, &exit_code) &&
                       exit_code == STILL_ACTIVE;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool has_times =
        GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!alive || !has_times)
        return std::nullopt;
    const std::uint64_t value =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
        created.dwLowDateTime;
    return std::to_string(value);
#elif defined(__APPLE__)
    int query[] = {
        CTL_KERN,
        KERN_PROC,
        KERN_PROC_PID,
        static_cast<int>(process_id),
    };
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    if (::sysctl(query, 4, &info, &size, nullptr, 0) != 0 ||
        size != sizeof(info) ||
        info.kp_proc.p_pid != static_cast<pid_t>(process_id) ||
        info.kp_proc.p_stat == SZOMB) {
        return std::nullopt;
    }
    return std::to_string(info.kp_proc.p_starttime.tv_sec) + ":" +
           std::to_string(info.kp_proc.p_starttime.tv_usec);
#elif defined(__linux__)
    std::ifstream stat_file(
        "/proc/" + std::to_string(process_id) + "/stat");
    std::string line;
    if (!std::getline(stat_file, line))
        return std::nullopt;
    const auto comm_end = line.rfind(')');
    if (comm_end == std::string::npos || comm_end + 2 >= line.size())
        return std::nullopt;
    std::istringstream fields(line.substr(comm_end + 2));
    std::string value;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value))
            return std::nullopt;
        if (field == 3 &&
            (value == "Z" || value == "X" || value == "x")) {
            return std::nullopt;
        }
    }
    return value;
#else
    // Publication must bind liveness to more than a reusable PID. Platforms
    // without a supported start-time identity fail closed.
    return std::nullopt;
#endif
}

} // namespace pulp::inspect::discovery_detail
