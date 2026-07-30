#include <pulp/inspect/discovery.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/system.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define getpid _getpid
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

bool ensure_private_directory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
#ifdef _WIN32
    return std::filesystem::is_directory(directory, error) && !error;
#else
    if (::chmod(directory.c_str(), 0700) != 0)
        return false;
    struct stat info {};
    return ::lstat(directory.c_str(), &info) == 0 &&
           S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode) &&
           info.st_uid == ::geteuid() && (info.st_mode & 077) == 0;
#endif
}

bool private_regular_file(const std::filesystem::path& path) {
#ifdef _WIN32
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
#else
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 &&
           S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode) &&
           info.st_uid == ::geteuid() && (info.st_mode & 077) == 0;
#endif
}

bool process_alive(std::int64_t process_id) {
    if (process_id <= 0)
        return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(process_id));
    if (!process)
        return false;
    DWORD exit_code = 0;
    const bool alive = GetExitCodeProcess(process, &exit_code) &&
                       exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    if (::kill(static_cast<pid_t>(process_id), 0) == 0)
        return true;
    return errno == EPERM;
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
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << contents))
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
#endif
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
#ifndef _WIN32
    if (::chmod(destination.c_str(), 0600) != 0)
        return false;
#endif
    return true;
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
        record.expires_at_unix_ms = value["expiresAtUnixMs"].getInt64();
        record.record_path = path;
        const auto credential_name =
            std::string(value["credentialFile"].getString());
        if (!safe_component(record.session_id) ||
            credential_name != record.session_id + ".token" ||
            record.endpoint.rfind("127.0.0.1:", 0) != 0 ||
            record.protocol_version != "1" ||
            record.expires_at_unix_ms <= unix_ms_now() ||
            !process_alive(record.process_id)) {
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
    if (!ensure_private_directory(runtime_directory_))
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
    if (!safe_component(record.session_id) || credential.size() != 32 ||
        ttl <= std::chrono::milliseconds(0) ||
        !ensure_private_directory(runtime_directory_)) {
        return false;
    }
    record.process_id = static_cast<std::int64_t>(getpid());
    record.expires_at_unix_ms = unix_ms_now() + ttl.count();
    record.record_path = runtime_directory_ / (record.session_id + ".json");
    record.credential_path =
        runtime_directory_ / (record.session_id + ".token");
    credential_.assign(credential.begin(), credential.end());
    if (!write_private_file_atomic(
            record.credential_path,
            pulp::runtime::hex_encode(credential_)) ||
        !write_private_file_atomic(record.record_path, encode_record(record))) {
        std::error_code error;
        std::filesystem::remove(record.record_path, error);
        std::filesystem::remove(record.credential_path, error);
        credential_.clear();
        return false;
    }
    record_ = std::move(record);
    return true;
}

bool InspectorDiscoveryPublisher::refresh(std::chrono::milliseconds ttl) {
    if (!record_ || credential_.size() != 32 ||
        ttl <= std::chrono::milliseconds(0))
        return false;
    record_->expires_at_unix_ms = unix_ms_now() + ttl.count();
    return write_private_file_atomic(record_->record_path,
                                     encode_record(*record_));
}

void InspectorDiscoveryPublisher::remove() {
    if (record_) {
        std::error_code error;
        std::filesystem::remove(record_->record_path, error);
        std::filesystem::remove(record_->credential_path, error);
        record_.reset();
    }
    std::fill(credential_.begin(), credential_.end(), std::uint8_t{0});
    credential_.clear();
}

std::optional<InspectorDiscoveryRecord> select_inspector_session(
    std::span<const InspectorDiscoveryRecord> records,
    std::string_view session_id,
    std::string* error) {
    if (!session_id.empty()) {
        const auto match =
            std::find_if(records.begin(), records.end(), [&](const auto& record) {
                return record.session_id == session_id;
            });
        if (match != records.end())
            return *match;
        if (error)
            *error = "No live inspector session matches the requested session ID";
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
