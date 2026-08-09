#include "control_peer_platform.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <libproc.h>
#include <signal.h>
#include <sys/proc.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace pulp::inspect::detail {
namespace {

template <typename T>
class CfOwner {
public:
    explicit CfOwner(T value = nullptr) : value_(value) {}
    ~CfOwner() {
        if (value_ != nullptr)
            CFRelease(value_);
    }
    CfOwner(const CfOwner&) = delete;
    CfOwner& operator=(const CfOwner&) = delete;
    T get() const { return value_; }

private:
    T value_;
};

std::string cf_string(CFTypeRef value) {
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
        return {};
    const auto string = static_cast<CFStringRef>(value);
    const auto length = CFStringGetLength(string);
    const auto capacity = CFStringGetMaximumSizeForEncoding(
                              length, kCFStringEncodingUTF8) +
                          1;
    if (capacity <= 1)
        return {};
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(
            string, result.data(), capacity, kCFStringEncodingUTF8)) {
        return {};
    }
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}

std::string cf_data_hex(CFTypeRef value) {
    if (value == nullptr || CFGetTypeID(value) != CFDataGetTypeID())
        return {};
    const auto data = static_cast<CFDataRef>(value);
    const auto size = CFDataGetLength(data);
    if (size <= 0)
        return {};
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(static_cast<std::size_t>(size) * 2, '0');
    const auto* bytes = CFDataGetBytePtr(data);
    for (CFIndex index = 0; index < size; ++index) {
        result[static_cast<std::size_t>(index) * 2] = digits[bytes[index] >> 4];
        result[static_cast<std::size_t>(index) * 2 + 1] =
            digits[bytes[index] & 0xf];
    }
    return result;
}

std::optional<ControlPeerEvidence> observe_signed_process(
    const runtime::LocalPeerCredentials& credentials,
    ControlPeerRole role) {
    if (credentials.process_id <= 0 ||
        credentials.process_generation_id == 0 ||
        credentials.process_id > std::numeric_limits<pid_t>::max()) {
        return std::nullopt;
    }
    const auto pid = static_cast<pid_t>(credentials.process_id);
    proc_bsdinfo process{};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &process, sizeof(process)) !=
            sizeof(process) ||
        process.pbi_pid != static_cast<std::uint32_t>(pid) ||
        process.pbi_uid != credentials.user_id ||
        process.pbi_gid != credentials.group_id || process.pbi_status == SZOMB) {
        return std::nullopt;
    }

    std::int64_t pid_value = credentials.process_id;
    CfOwner<CFNumberRef> pid_number(CFNumberCreate(
        kCFAllocatorDefault, kCFNumberSInt64Type, &pid_value));
    if (pid_number.get() == nullptr)
        return std::nullopt;
    const void* keys[] = {kSecGuestAttributePid};
    const void* values[] = {pid_number.get()};
    CfOwner<CFDictionaryRef> attributes(CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (attributes.get() == nullptr)
        return std::nullopt;

    SecCodeRef raw_code = nullptr;
    if (SecCodeCopyGuestWithAttributes(
            nullptr, attributes.get(), kSecCSDefaultFlags, &raw_code) !=
            errSecSuccess ||
        raw_code == nullptr) {
        return std::nullopt;
    }
    CfOwner<SecCodeRef> code(raw_code);
    if (SecCodeCheckValidity(code.get(), kSecCSStrictValidate, nullptr) !=
        errSecSuccess) {
        return std::nullopt;
    }

    CFDictionaryRef raw_information = nullptr;
    if (SecCodeCopySigningInformation(
            code.get(), kSecCSSigningInformation, &raw_information) !=
            errSecSuccess ||
        raw_information == nullptr) {
        return std::nullopt;
    }
    CfOwner<CFDictionaryRef> information(raw_information);
    const auto identifier = cf_string(CFDictionaryGetValue(
        information.get(), kSecCodeInfoIdentifier));
    const auto team = cf_string(CFDictionaryGetValue(
        information.get(), kSecCodeInfoTeamIdentifier));
    const auto cdhash = cf_data_hex(CFDictionaryGetValue(
        information.get(), kSecCodeInfoUnique));
    if (identifier.empty() || cdhash.empty())
        return std::nullopt;

    proc_bsdinfo current_process{};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &current_process,
                       sizeof(current_process)) != sizeof(current_process) ||
        current_process.pbi_pid != process.pbi_pid ||
        current_process.pbi_uid != process.pbi_uid ||
        current_process.pbi_gid != process.pbi_gid ||
        current_process.pbi_start_tvsec != process.pbi_start_tvsec ||
        current_process.pbi_start_tvusec != process.pbi_start_tvusec ||
        current_process.pbi_status == SZOMB) {
        return std::nullopt;
    }

    ControlPeerEvidence evidence;
    evidence.role = role;
    evidence.user_id = "uid:" + std::to_string(credentials.user_id);
    evidence.process_id = credentials.process_id;
    evidence.process_start_id =
        "pidversion:" + std::to_string(credentials.process_generation_id) +
        ":start:" + std::to_string(process.pbi_start_tvsec) + ":" +
        std::to_string(process.pbi_start_tvusec);
    evidence.executable_identity = "signed:" + identifier + ":" + cdhash;
    evidence.publisher_id = team.empty() ? "adhoc:" + cdhash
                                         : "team:" + team;
    return evidence;
}

} // namespace

std::optional<ControlPeerEvidence> observe_platform_control_peer(
    const runtime::LocalPeerCredentials& credentials,
    ControlPeerRole role) {
    return observe_signed_process(credentials, role);
}

ControlProcessLiveness platform_control_peer_process_liveness(
    const ControlPeerEvidence& evidence) {
    if (evidence.process_id <= 0 ||
        evidence.process_id > std::numeric_limits<pid_t>::max())
        return ControlProcessLiveness::Dead;
    const auto pid = static_cast<pid_t>(evidence.process_id);
    proc_bsdinfo process{};
    if (::proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &process, sizeof(process)) !=
        sizeof(process)) {
        errno = 0;
        if (::kill(pid, 0) != 0 && errno == ESRCH)
            return ControlProcessLiveness::Dead;
        return ControlProcessLiveness::Unknown;
    }
    if (process.pbi_pid != static_cast<std::uint32_t>(pid) || process.pbi_status == SZOMB)
        return ControlProcessLiveness::Dead;
    const auto suffix = ":start:" + std::to_string(process.pbi_start_tvsec) + ":" +
                        std::to_string(process.pbi_start_tvusec);
    if (evidence.process_start_id.find(":start:") == std::string::npos)
        return ControlProcessLiveness::Unknown;
    return evidence.process_start_id.ends_with(suffix) ? ControlProcessLiveness::Alive
                                                       : ControlProcessLiveness::Dead;
}

} // namespace pulp::inspect::detail
