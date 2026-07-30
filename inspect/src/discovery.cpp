#include <pulp/inspect/discovery.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/system.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <aclapi.h>
#include <process.h>
#include <windows.h>
#define getpid _getpid
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <libproc.h>
#endif
#endif
#endif

namespace pulp::inspect {
namespace {

using Clock = std::chrono::system_clock;

std::int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
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

std::string discovery_file_stem(std::string_view session_id,
                                std::string_view instance_id) {
    return std::to_string(session_id.size()) + "-" +
           std::string(session_id) + "-" + std::string(instance_id);
}

#ifdef _WIN32
class OwnerOnlySecurity {
public:
    OwnerOnlySecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        token_user_.resize(size);
        if (size == 0 ||
            !GetTokenInformation(token, TokenUser, token_user_.data(), size,
                                 &size)) {
            CloseHandle(token);
            token_user_.clear();
            return;
        }
        CloseHandle(token);

        auto* user = reinterpret_cast<TOKEN_USER*>(token_user_.data());
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName =
            static_cast<wchar_t*>(user->User.Sid);
        if (SetEntriesInAclW(1, &entry, nullptr, &acl_) != ERROR_SUCCESS ||
            !InitializeSecurityDescriptor(&descriptor_,
                                          SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED,
                                          SE_DACL_PROTECTED)) {
            if (acl_)
                LocalFree(acl_);
            acl_ = nullptr;
            return;
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        valid_ = true;
    }

    ~OwnerOnlySecurity() {
        if (acl_)
            LocalFree(acl_);
    }

    OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
    OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;

    bool valid() const { return valid_; }
    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }
    PSID user_sid() const {
        if (token_user_.empty())
            return nullptr;
        return reinterpret_cast<const TOKEN_USER*>(token_user_.data())
            ->User.Sid;
    }

private:
    std::vector<std::uint8_t> token_user_;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_ = false;
};

bool owner_only_windows_path(const std::filesystem::path& path,
                             bool expect_directory) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != expect_directory) {
        return false;
    }

    OwnerOnlySecurity expected;
    if (!expected.valid())
        return false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS || !owner || !dacl ||
        !EqualSid(owner, expected.user_sid())) {
        if (descriptor)
            LocalFree(descriptor);
        return false;
    }

    ACL_SIZE_INFORMATION acl_info{};
    bool secure = GetAclInformation(
        dacl, &acl_info, sizeof(acl_info), AclSizeInformation) != FALSE;
    for (DWORD index = 0; secure && index < acl_info.AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace)) {
            secure = false;
            break;
        }
        const auto* header = static_cast<ACE_HEADER*>(raw_ace);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
            PSID sid = const_cast<DWORD*>(&ace->SidStart);
            if (!EqualSid(sid, expected.user_sid()))
                secure = false;
        } else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
                   header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                   header->AceType ==
                       ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            secure = false;
        }
    }
    LocalFree(descriptor);
    return secure;
}

bool create_owner_only_windows_directory(
    const std::filesystem::path& directory) {
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return owner_only_windows_path(directory, true);

    const auto parent = directory.parent_path();
    if (!parent.empty() &&
        GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !create_owner_only_windows_directory(parent)) {
        return false;
    }

    OwnerOnlySecurity security;
    if (!security.valid())
        return false;
    if (!CreateDirectoryW(directory.c_str(), security.attributes()) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    return owner_only_windows_path(directory, true);
}
#endif

bool ensure_private_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
    return create_owner_only_windows_directory(directory);
#else
    std::error_code error;
    const auto parent = directory.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, error);
    if (error ||
        (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)) {
        return false;
    }

    struct stat before {};
    if (::lstat(directory.c_str(), &before) != 0 ||
        !S_ISDIR(before.st_mode) || S_ISLNK(before.st_mode) ||
        before.st_uid != ::geteuid()) {
        return false;
    }

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(directory.c_str(), flags);
    if (descriptor < 0)
        return false;
    struct stat opened {};
    const bool same_directory =
        ::fstat(descriptor, &opened) == 0 &&
        S_ISDIR(opened.st_mode) &&
        opened.st_uid == ::geteuid() &&
        opened.st_dev == before.st_dev &&
        opened.st_ino == before.st_ino;
    const bool secured =
        same_directory && ::fchmod(descriptor, 0700) == 0 &&
        ::fstat(descriptor, &opened) == 0 &&
        S_ISDIR(opened.st_mode) &&
        opened.st_uid == ::geteuid() &&
        (opened.st_mode & 077) == 0;
    ::close(descriptor);
    return secured;
#endif
}

bool validate_private_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
    return owner_only_windows_path(directory, true);
#else
    struct stat info {};
    return ::lstat(directory.c_str(), &info) == 0 &&
           S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode) &&
           info.st_uid == ::geteuid() && (info.st_mode & 077) == 0;
#endif
}

bool private_regular_file(const std::filesystem::path& path) {
#ifdef _WIN32
    return owner_only_windows_path(path, false);
#else
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 &&
           S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode) &&
           info.st_uid == ::geteuid() && (info.st_mode & 077) == 0;
#endif
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
#elif defined(__APPLE__) && TARGET_OS_OSX
    proc_bsdinfo info{};
    if (proc_pidinfo(static_cast<int>(process_id), PROC_PIDTBSDINFO, 0,
                     &info, sizeof(info)) != sizeof(info)) {
        return std::nullopt;
    }
    return std::to_string(info.pbi_start_tvsec) + ":" +
           std::to_string(info.pbi_start_tvusec);
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
    if (::kill(static_cast<pid_t>(process_id), 0) == 0)
        return std::to_string(process_id);
    if (errno == EPERM)
        return std::to_string(process_id);
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> confined_path(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error)
        return std::nullopt;
    const auto canonical_candidate =
        std::filesystem::weakly_canonical(candidate, error);
    if (error || canonical_candidate.parent_path() != canonical_root)
        return std::nullopt;
    return canonical_candidate;
}

bool write_private_file_atomic(const std::filesystem::path& destination,
                               std::string_view contents) {
    const auto random = pulp::runtime::secure_random_bytes(8);
    if (!random)
        return false;
    const auto temporary = destination.string() + ".tmp-" +
                           pulp::runtime::hex_encode(*random);
#ifdef _WIN32
    OwnerOnlySecurity security;
    if (!security.valid())
        return false;
    HANDLE file = CreateFileW(
        std::filesystem::path(temporary).c_str(), GENERIC_WRITE, 0,
        security.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::size_t written = 0;
    bool succeeded = true;
    while (written < contents.size()) {
        const auto remaining = std::min<std::size_t>(
            contents.size() - written,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
        DWORD count = 0;
        if (!WriteFile(file, contents.data() + written,
                       static_cast<DWORD>(remaining), &count, nullptr) ||
            count == 0) {
            succeeded = false;
            break;
        }
        written += count;
    }
    succeeded = succeeded && FlushFileBuffers(file);
    CloseHandle(file);
    if (!succeeded ||
        !MoveFileExW(std::filesystem::path(temporary).c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(std::filesystem::path(temporary).c_str());
        return false;
    }
#else
    const int fd = ::open(temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                          0600);
    if (fd < 0)
        return false;
    std::size_t written = 0;
    while (written < contents.size()) {
        const auto count =
            ::write(fd, contents.data() + written, contents.size() - written);
        if (count <= 0) {
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    if (::chmod(destination.c_str(), 0600) != 0)
        return false;
#endif
    return true;
}

std::optional<std::string> read_private_text_file(
    const std::filesystem::path& path) {
    if (!private_regular_file(path))
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    if ((!input.good() && !input.eof()) || contents.size() > 1024)
        return std::nullopt;
    return contents;
}

std::string encode_record(const InspectorDiscoveryRecord& record) {
    auto value = choc::value::createObject("");
    value.addMember("sessionId",
                    choc::value::createString(record.session_id));
    value.addMember("instanceId",
                    choc::value::createString(record.instance_id));
    value.addMember("pluginId", choc::value::createString(record.plugin_id));
    value.addMember("endpoint", choc::value::createString(record.endpoint));
    value.addMember("protocolVersion",
                    choc::value::createString(record.protocol_version));
    value.addMember("profile",
                    choc::value::createString(profile_id(record.profile)));
    value.addMember("pid", choc::value::createInt64(record.process_id));
    value.addMember("processStartId",
                    choc::value::createString(record.process_start_id));
    value.addMember("expiresAtUnixMs",
                    choc::value::createInt64(record.expires_at_unix_ms));
    value.addMember("credentialFile",
                    choc::value::createString(
                        record.credential_path.filename().string()));
    return choc::json::toString(value, false);
}

std::optional<InspectorDiscoveryRecord> decode_record(
    const std::filesystem::path& root,
    const std::filesystem::path& path) {
    if (!private_regular_file(path))
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof())
        return std::nullopt;
    try {
        const auto value = choc::json::parse(json);
        InspectorDiscoveryRecord record;
        record.session_id = std::string(value["sessionId"].getString());
        record.instance_id = std::string(value["instanceId"].getString());
        record.plugin_id = std::string(value["pluginId"].getString());
        record.endpoint = std::string(value["endpoint"].getString());
        record.protocol_version =
            std::string(value["protocolVersion"].getString());
        const auto profile =
            profile_from_id(value["profile"].getString());
        if (!profile)
            return std::nullopt;
        record.profile = *profile;
        record.process_id = value["pid"].getInt64();
        record.process_start_id =
            std::string(value["processStartId"].getString());
        record.expires_at_unix_ms = value["expiresAtUnixMs"].getInt64();
        record.record_path = path;
        const auto credential_name =
            std::string(value["credentialFile"].getString());
        const auto file_stem =
            discovery_file_stem(record.session_id, record.instance_id);
        if (!safe_component(record.session_id) ||
            !safe_component(record.instance_id) ||
            path.filename() != file_stem + ".json" ||
            credential_name != file_stem + ".token" ||
            record.endpoint.rfind("127.0.0.1:", 0) != 0 ||
            record.protocol_version != "1" ||
            record.expires_at_unix_ms <= unix_ms_now() ||
            process_start_identity(record.process_id) !=
                std::optional<std::string>(record.process_start_id)) {
            return std::nullopt;
        }
        const auto credential =
            confined_path(root, root / credential_name);
        if (!credential)
            return std::nullopt;
        record.credential_path = *credential;
        return record;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

struct InspectorDiscoveryPublisher::OwnershipLease {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int descriptor = -1;
#endif

    ~OwnershipLease() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
#else
        if (descriptor >= 0) {
            ::flock(descriptor, LOCK_UN);
            ::close(descriptor);
        }
#endif
    }

    static std::unique_ptr<OwnershipLease> acquire(
        const std::filesystem::path& path,
        std::string_view marker) {
        auto lease = std::make_unique<OwnershipLease>();
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            !owner_only_windows_path(path, false)) {
            return nullptr;
        }
        OwnerOnlySecurity security;
        if (!security.valid())
            return nullptr;
        lease->handle = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            security.attributes(), OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (lease->handle == INVALID_HANDLE_VALUE)
            return nullptr;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(lease->handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                      FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            return nullptr;
        }
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(lease->handle, start, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(lease->handle)) {
            return nullptr;
        }
        std::size_t written = 0;
        while (written < marker.size()) {
            const auto remaining = std::min<std::size_t>(
                marker.size() - written,
                static_cast<std::size_t>(
                    std::numeric_limits<DWORD>::max()));
            DWORD count = 0;
            if (!WriteFile(lease->handle, marker.data() + written,
                           static_cast<DWORD>(remaining), &count, nullptr) ||
                count == 0) {
                return nullptr;
            }
            written += count;
        }
        if (!FlushFileBuffers(lease->handle))
            return nullptr;
#else
        int flags = O_RDWR | O_CREAT | O_CLOEXEC;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        lease->descriptor = ::open(path.c_str(), flags, 0600);
        if (lease->descriptor < 0 ||
            ::flock(lease->descriptor, LOCK_EX | LOCK_NB) != 0) {
            return nullptr;
        }
        struct stat info {};
        if (::fstat(lease->descriptor, &info) != 0 ||
            !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
            ::fchmod(lease->descriptor, 0600) != 0 ||
            ::ftruncate(lease->descriptor, 0) != 0) {
            return nullptr;
        }
        std::size_t written = 0;
        while (written < marker.size()) {
            const auto count =
                ::write(lease->descriptor, marker.data() + written,
                        marker.size() - written);
            if (count <= 0)
                return nullptr;
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(lease->descriptor) != 0)
            return nullptr;
#endif
        return lease;
    }
};

std::filesystem::path default_inspector_runtime_directory() {
    if (const auto configured =
            pulp::runtime::get_env("PULP_INSPECTOR_RUNTIME_DIR")) {
        return *configured;
    }
#ifdef _WIN32
    const auto base = pulp::runtime::get_env("LOCALAPPDATA")
                          .value_or(std::filesystem::temp_directory_path().string());
    return std::filesystem::path(base) / "Pulp" / "Inspector";
#else
    const auto base = pulp::runtime::get_env("TMPDIR")
                          .value_or(std::filesystem::temp_directory_path().string());
    return std::filesystem::path(base) /
           ("pulp-inspector-" + std::to_string(::geteuid()));
#endif
}

InspectorDiscoveryReader::InspectorDiscoveryReader(
    std::filesystem::path runtime_directory)
    : runtime_directory_(std::move(runtime_directory)) {}

std::vector<InspectorDiscoveryRecord> InspectorDiscoveryReader::list() const {
    std::vector<InspectorDiscoveryRecord> records;
    if (!validate_private_directory(runtime_directory_))
        return records;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(runtime_directory_, error)) {
        if (error)
            break;
        const auto filename = entry.path().filename().string();
        if (filename.size() <= 5 ||
            filename.substr(filename.size() - 5) != ".json")
            continue;
        if (auto record = decode_record(runtime_directory_, entry.path()))
            records.push_back(std::move(*record));
    }
    std::sort(records.begin(), records.end(), [](const auto& left,
                                                  const auto& right) {
        return left.session_id < right.session_id;
    });
    return records;
}

std::optional<std::vector<std::uint8_t>>
InspectorDiscoveryReader::read_credential(
    const InspectorDiscoveryRecord& record) const {
    if (!validate_private_directory(runtime_directory_))
        return std::nullopt;
    const auto path = confined_path(runtime_directory_, record.credential_path);
    if (!path || !private_regular_file(*path))
        return std::nullopt;
    std::ifstream input(*path, std::ios::binary);
    std::string hex((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
    if (hex.size() != 64)
        return std::nullopt;
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> result(32);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const int high = nibble(hex[index * 2]);
        const int low = nibble(hex[index * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

InspectorDiscoveryPublisher::InspectorDiscoveryPublisher(
    std::filesystem::path runtime_directory)
    : runtime_directory_(std::move(runtime_directory)) {}

InspectorDiscoveryPublisher::~InspectorDiscoveryPublisher() {
    remove();
}

bool InspectorDiscoveryPublisher::publish(
    InspectorDiscoveryRecord record,
    std::span<const std::uint8_t> credential,
    std::chrono::milliseconds ttl) {
    remove();
    if (!safe_component(record.session_id) ||
        !safe_component(record.instance_id) || credential.size() != 32 ||
        ttl <= std::chrono::milliseconds(0) ||
        !ensure_private_directory(runtime_directory_)) {
        return false;
    }
    record.process_id = static_cast<std::int64_t>(getpid());
    const auto process_start_id = process_start_identity(record.process_id);
    if (!process_start_id)
        return false;
    record.process_start_id = *process_start_id;
    record.expires_at_unix_ms = unix_ms_now() + ttl.count();
    const auto file_stem =
        discovery_file_stem(record.session_id, record.instance_id);
    record.record_path = runtime_directory_ / (file_stem + ".json");
    record.credential_path =
        runtime_directory_ / (file_stem + ".token");
    ownership_path_ = runtime_directory_ / (file_stem + ".lock");
    const auto ownership_id = pulp::runtime::secure_random_bytes(16);
    if (!ownership_id)
        return false;
    ownership_marker_ =
        std::to_string(record.process_id) + "\n" +
        record.process_start_id + "\n" +
        pulp::runtime::hex_encode(*ownership_id);
    ownership_ =
        OwnershipLease::acquire(ownership_path_, ownership_marker_);
    if (!ownership_) {
        ownership_path_.clear();
        ownership_marker_.clear();
        return false;
    }
    std::error_code error;
    std::filesystem::remove(record.record_path, error);
    std::filesystem::remove(record.credential_path, error);
    credential_.assign(credential.begin(), credential.end());
    if (!write_private_file_atomic(
            record.credential_path,
            pulp::runtime::hex_encode(credential_)) ||
        !write_private_file_atomic(record.record_path, encode_record(record))) {
        std::error_code error;
        std::filesystem::remove(record.record_path, error);
        std::filesystem::remove(record.credential_path, error);
        ownership_.reset();
        ownership_path_.clear();
        ownership_marker_.clear();
        credential_.clear();
        return false;
    }
    record_ = std::move(record);
    return true;
}

bool InspectorDiscoveryPublisher::refresh(std::chrono::milliseconds ttl) {
    if (!record_ || !ownership_ || credential_.size() != 32 ||
        ttl <= std::chrono::milliseconds(0) ||
        read_private_text_file(ownership_path_) !=
            std::optional<std::string>(ownership_marker_))
        return false;
    record_->expires_at_unix_ms = unix_ms_now() + ttl.count();
    return write_private_file_atomic(record_->record_path,
                                     encode_record(*record_));
}

void InspectorDiscoveryPublisher::remove() {
    if (record_ && ownership_ &&
        read_private_text_file(ownership_path_) ==
            std::optional<std::string>(ownership_marker_)) {
        std::error_code error;
        std::filesystem::remove(record_->record_path, error);
        std::filesystem::remove(record_->credential_path, error);
    }
    record_.reset();
    ownership_.reset();
    ownership_path_.clear();
    ownership_marker_.clear();
    std::fill(credential_.begin(), credential_.end(), std::uint8_t{0});
    credential_.clear();
}

std::optional<InspectorDiscoveryRecord> select_inspector_session(
    std::span<const InspectorDiscoveryRecord> records,
    std::string_view session_id,
    std::string_view instance_id,
    std::string* error) {
    if (!session_id.empty() || !instance_id.empty()) {
        std::optional<InspectorDiscoveryRecord> match;
        for (const auto& record : records) {
            if ((!session_id.empty() && record.session_id != session_id) ||
                (!instance_id.empty() && record.instance_id != instance_id))
                continue;
            if (match) {
                if (error)
                    *error = "Multiple live inspector instances match the "
                             "requested selection";
                return std::nullopt;
            }
            match = record;
        }
        if (match)
            return match;
        if (error)
            *error = "No live inspector session matches the requested selection";
        return std::nullopt;
    }
    if (records.size() == 1)
        return records.front();
    if (error) {
        *error = records.empty()
                     ? "No live inspector sessions were discovered"
                     : "Multiple inspector sessions are live; specify a session ID";
    }
    return std::nullopt;
}

} // namespace pulp::inspect
