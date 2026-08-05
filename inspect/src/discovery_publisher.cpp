#include <pulp/inspect/discovery_publisher.hpp>

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


namespace {

using namespace discovery_detail;
using namespace discovery_security;

struct SensitiveText {
    std::string value;
    ~SensitiveText() {
        pulp::runtime::secure_zero_memory(value.data(), value.size());
    }
};


std::optional<std::int64_t> expiry_after(
    std::chrono::milliseconds ttl) {
    const auto now = unix_ms_now();
    if (ttl <= std::chrono::milliseconds(0) || now < 0 ||
        ttl.count() >
            std::numeric_limits<std::int64_t>::max() - now) {
        return std::nullopt;
    }
    return now + ttl.count();
}




std::string encode_record(const InspectorDiscoveryRecord& record) {
    auto value = choc::value::createObject("");
    value.addMember("sessionId",
                    choc::value::createString(record.session_id));
    value.addMember("instanceId",
                    choc::value::createString(record.instance_id));
    value.addMember("publicationId",
                    choc::value::createString(record.publication_id));
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


} // namespace

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
    if (!prepare(std::move(record), credential, ttl))
        return false;
    if (commit())
        return true;
    remove();
    return false;
}

bool InspectorDiscoveryPublisher::prepare(
    InspectorDiscoveryRecord record,
    std::span<const std::uint8_t> credential,
    std::chrono::milliseconds ttl) {
    remove();
    const auto expires_at = expiry_after(ttl);
    if (!safe_component(record.session_id) ||
        !safe_component(record.instance_id) ||
        !valid_loopback_endpoint(record.endpoint) ||
        credential.size() != 32 ||
        !expires_at ||
        !ensure_private_directory(runtime_directory_)) {
        return false;
    }
    record.process_id = static_cast<std::int64_t>(getpid());
    const auto publication_id = pulp::runtime::secure_random_bytes(16);
    if (!publication_id)
        return false;
    record.publication_id = pulp::runtime::hex_encode(*publication_id);
    const auto process_start_id = process_start_identity(record.process_id);
    if (!process_start_id)
        return false;
    record.process_start_id = *process_start_id;
    record.expires_at_unix_ms = *expires_at;
    const auto file_stem =
        discovery_file_stem(record.session_id, record.instance_id);
    record.record_path = runtime_directory_ / (file_stem + ".json");
    record.credential_path =
        runtime_directory_ / (file_stem + ".token");
    const auto encoded_record = encode_record(record);
    if (encoded_record.size() > kMaxDiscoveryRecordBytes)
        return false;
    ownership_path_ = runtime_directory_ / (file_stem + ".lock");
    ownership_marker_ =
        std::to_string(record.process_id) + "\n" +
        record.process_start_id + "\n" +
        record.publication_id;
    ownership_ =
        discovery_security::OwnershipLease::acquire(
            ownership_path_, ownership_marker_);
    if (!ownership_) {
        ownership_path_.clear();
        ownership_marker_.clear();
        return false;
    }
    std::error_code error;
    std::filesystem::remove(record.record_path, error);
    std::filesystem::remove(record.credential_path, error);
    credential_.assign(credential.begin(), credential.end());
    record_ = std::move(record);
    committed_ = false;
    return true;
}

bool InspectorDiscoveryPublisher::commit() {
    if (!record_ || !ownership_ || credential_.size() != 32 || committed_)
        return false;
    SensitiveText encoded_credential{
        pulp::runtime::hex_encode(credential_)};
    const auto encoded_record = encode_record(*record_);
    if (encoded_record.size() > kMaxDiscoveryRecordBytes ||
        !write_private_file_atomic(record_->credential_path,
                                   encoded_credential.value) ||
        !write_private_file_atomic(record_->record_path, encoded_record)) {
        hide();
        return false;
    }
    committed_ = true;
    return true;
}

bool InspectorDiscoveryPublisher::refresh(std::chrono::milliseconds ttl) {
    const auto expires_at = expiry_after(ttl);
    if (!record_ || !ownership_ || !committed_ ||
        credential_.size() != 32 ||
        !expires_at ||
        read_private_text_file(ownership_path_) !=
            std::optional<std::string>(ownership_marker_))
        return false;
    record_->expires_at_unix_ms = *expires_at;
    return write_private_file_atomic(record_->record_path,
                                     encode_record(*record_));
}

bool InspectorDiscoveryPublisher::hide() {
    if (!record_) {
        committed_ = false;
        return true;
    }
    std::error_code record_exists_error;
    const bool record_exists = std::filesystem::exists(
        record_->record_path, record_exists_error);
    std::error_code credential_exists_error;
    const bool credential_exists = std::filesystem::exists(
        record_->credential_path, credential_exists_error);
    if (record_exists_error || credential_exists_error)
        return false;
    if (!record_exists && !credential_exists) {
        committed_ = false;
        return true;
    }
    if (!ownership_ ||
        read_private_text_file(ownership_path_) !=
            std::optional<std::string>(ownership_marker_)) {
        return false;
    }
    std::error_code record_error;
    std::filesystem::remove(record_->record_path, record_error);
    std::error_code credential_error;
    std::filesystem::remove(
        record_->credential_path, credential_error);
    std::error_code record_remains_error;
    const bool record_remains = std::filesystem::exists(
        record_->record_path, record_remains_error);
    std::error_code credential_remains_error;
    const bool credential_remains = std::filesystem::exists(
        record_->credential_path, credential_remains_error);
    const bool hidden = !record_error && !credential_error &&
        !record_remains_error && !credential_remains_error &&
        !record_remains && !credential_remains;
    if (hidden)
        committed_ = false;
    return hidden;
}

void InspectorDiscoveryPublisher::remove() {
    (void)hide();
    record_.reset();
    ownership_.reset();
    ownership_path_.clear();
    ownership_marker_.clear();
    pulp::runtime::secure_zero_memory(
        credential_.data(), credential_.size());
    credential_.clear();
}


} // namespace pulp::inspect
