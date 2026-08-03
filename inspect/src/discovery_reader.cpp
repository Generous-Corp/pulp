#include <pulp/inspect/discovery.hpp>

#include "discovery_internal.hpp"
#include "discovery_security.hpp"

#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
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
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif
#endif

namespace pulp::inspect {

InspectorCredential::InspectorCredential(std::span<const std::uint8_t> bytes)
    : bytes_(bytes.begin(), bytes.end()) {}

InspectorCredential::~InspectorCredential() {
    clear();
}

InspectorCredential::InspectorCredential(InspectorCredential&& other) noexcept
    : bytes_(std::move(other.bytes_)) {
    other.clear();
}

InspectorCredential& InspectorCredential::operator=(InspectorCredential&& other) noexcept {
    if (this != &other) {
        clear();
        bytes_ = std::move(other.bytes_);
        other.clear();
    }
    return *this;
}

void InspectorCredential::clear() noexcept {
    pulp::runtime::secure_zero_memory(bytes_.data(), bytes_.size());
    bytes_.clear();
}

namespace {

using namespace discovery_detail;
using namespace discovery_security;

#ifdef _WIN32
#endif

#ifndef _WIN32
#endif

bool validate_private_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
    return owner_private_path(directory, true);
#else
    const int descriptor = open_owner_private(directory, true);
    if (descriptor < 0)
        return false;
    ::close(descriptor);
    return true;
#endif
}

std::optional<std::filesystem::path> confined_path(const std::filesystem::path& root,
                                                   const std::filesystem::path& candidate) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error)
        return std::nullopt;
    const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, error);
    if (error || canonical_candidate.parent_path() != canonical_root)
        return std::nullopt;
    return canonical_candidate;
}

std::optional<InspectorDiscoveryRecord> decode_record(const std::filesystem::path& root,
                                                      const std::filesystem::path& path) {
    const auto json = read_private_text_file(path);
    if (!json)
        return std::nullopt;
    try {
        const auto value = choc::json::parse(*json);
        InspectorDiscoveryRecord record;
        record.session_id = std::string(value["sessionId"].getString());
        record.instance_id = std::string(value["instanceId"].getString());
        record.publication_id = std::string(value["publicationId"].getString());
        record.plugin_id = std::string(value["pluginId"].getString());
        record.endpoint = std::string(value["endpoint"].getString());
        record.protocol_version = std::string(value["protocolVersion"].getString());
        const auto profile = profile_from_id(value["profile"].getString());
        if (!profile)
            return std::nullopt;
        record.profile = *profile;
        record.process_id = value["pid"].getInt64();
        record.process_start_id = std::string(value["processStartId"].getString());
        record.expires_at_unix_ms = value["expiresAtUnixMs"].getInt64();
        record.record_path = path;
        const auto credential_name = std::string(value["credentialFile"].getString());
        const auto file_stem = discovery_file_stem(record.session_id, record.instance_id);
        if (!safe_component(record.session_id) || !safe_component(record.instance_id) ||
            !safe_component(record.publication_id) || path.filename() != file_stem + ".json" ||
            credential_name != file_stem + ".token" || !valid_loopback_endpoint(record.endpoint) ||
            record.protocol_version != "1" || record.expires_at_unix_ms <= unix_ms_now() ||
            process_start_identity(record.process_id) !=
                std::optional<std::string>(record.process_start_id)) {
            return std::nullopt;
        }
        const auto credential = confined_path(root, root / credential_name);
        if (!credential)
            return std::nullopt;
        record.credential_path = *credential;
        return record;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

InspectorDiscoveryReader::InspectorDiscoveryReader(std::filesystem::path runtime_directory)
    : runtime_directory_(std::move(runtime_directory)) {}

std::vector<InspectorDiscoveryRecord>
InspectorDiscoveryReader::list(std::string* diagnostic) const {
    std::vector<InspectorDiscoveryRecord> records;
    if (diagnostic)
        diagnostic->clear();
    if (!validate_private_directory(runtime_directory_)) {
        if (diagnostic) {
            std::error_code status_error;
            const auto status = std::filesystem::symlink_status(runtime_directory_, status_error);
            if (status_error == std::make_error_code(std::errc::no_such_file_or_directory) ||
                (!status_error && !std::filesystem::exists(status))) {
                return records;
            }
            if (status_error) {
                *diagnostic = "could not inspect runtime directory: " + status_error.message();
            } else {
                *diagnostic = "runtime directory is not an owner-private directory";
            }
        }
        return records;
    }
    std::error_code error;
    auto iterator = std::filesystem::directory_iterator(runtime_directory_, error);
    const auto end = std::filesystem::directory_iterator{};
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        const auto filename = entry.path().filename().string();
        if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json") {
            if (auto record = decode_record(runtime_directory_, entry.path());
                record && read_credential(*record).has_value()) {
                records.push_back(std::move(*record));
            }
        }
        iterator.increment(error);
    }
    if (error) {
        records.clear();
        if (diagnostic)
            *diagnostic = "could not read runtime directory: " + error.message();
    }
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.session_id < right.session_id;
    });
    return records;
}

std::optional<InspectorCredential>
InspectorDiscoveryReader::read_credential(const InspectorDiscoveryRecord& record) const {
    if (!validate_private_directory(runtime_directory_))
        return std::nullopt;
    const auto current = decode_record(runtime_directory_, record.record_path);
    if (!current || current->session_id != record.session_id ||
        current->instance_id != record.instance_id ||
        current->publication_id != record.publication_id ||
        current->process_id != record.process_id ||
        current->process_start_id != record.process_start_id ||
        current->credential_path != record.credential_path)
        return std::nullopt;
    const auto path = confined_path(runtime_directory_, record.credential_path);
    if (!path)
        return std::nullopt;
    auto contents = read_private_text_file(*path);
    if (!contents || contents->size() != 64)
        return std::nullopt;
    struct SensitiveHex {
        std::array<char, 64> bytes{};
        ~SensitiveHex() {
            pulp::runtime::secure_zero_memory(bytes.data(), bytes.size());
        }
    } hex;
    std::copy(contents->begin(), contents->end(), hex.bytes.begin());
    pulp::runtime::secure_zero_memory(contents->data(), contents->size());
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    };
    struct SensitiveBytes {
        std::array<std::uint8_t, 32> bytes{};
        ~SensitiveBytes() {
            pulp::runtime::secure_zero_memory(bytes.data(), bytes.size());
        }
    } result;
    for (std::size_t index = 0; index < result.bytes.size(); ++index) {
        const int high = nibble(hex.bytes[index * 2]);
        const int low = nibble(hex.bytes[index * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        result.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return InspectorCredential(result.bytes);
}

std::optional<InspectorDiscoveryRecord>
select_inspector_session(std::span<const InspectorDiscoveryRecord> records,
                         std::string_view session_id, std::string_view instance_id,
                         std::string_view publication_id, std::string* error) {
    if (!session_id.empty() || !instance_id.empty() || !publication_id.empty()) {
        std::optional<InspectorDiscoveryRecord> match;
        for (const auto& record : records) {
            if ((!session_id.empty() && record.session_id != session_id) ||
                (!instance_id.empty() && record.instance_id != instance_id) ||
                (!publication_id.empty() && record.publication_id != publication_id))
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
        *error = records.empty() ? "No live inspector sessions were discovered"
                                 : "Multiple inspector sessions are live; specify a session ID";
    }
    return std::nullopt;
}

} // namespace pulp::inspect
