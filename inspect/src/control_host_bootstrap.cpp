#include <pulp/inspect/control_host_bootstrap.hpp>

#include "control_protocol_internal.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::inspect {
namespace {

using control_protocol_detail::ValueView;
using namespace std::chrono_literals;

constexpr std::size_t kMaximumPathBytes = 4096;
constexpr std::size_t kMaximumPeerFieldBytes = 4096;

void set_diagnostics(ControlHostBootstrapDiagnostics* diagnostics,
                     ControlHostBootstrapStatus status, std::string explanation) {
    if (diagnostics)
        *diagnostics = {.status = status, .explanation = std::move(explanation)};
}

std::string_view role_id(ControlPeerRole role) {
    switch (role) {
    case ControlPeerRole::Client:
        return "client";
    case ControlPeerRole::OfflineHost:
        return "offline-host";
    case ControlPeerRole::StandaloneHost:
        return "standalone-host";
    case ControlPeerRole::TrustedHostBridge:
        return "trusted-host-bridge";
    }
    return {};
}

std::optional<ControlPeerRole> parse_role(std::string_view role) {
    if (role == "client")
        return ControlPeerRole::Client;
    if (role == "offline-host")
        return ControlPeerRole::OfflineHost;
    if (role == "standalone-host")
        return ControlPeerRole::StandaloneHost;
    if (role == "trusted-host-bridge")
        return ControlPeerRole::TrustedHostBridge;
    return std::nullopt;
}

std::int64_t unix_ms(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

bool valid_record(const ControlHostBootstrapRecord& record,
                  std::chrono::system_clock::time_point now,
                  ControlHostBootstrapDiagnostics* diagnostics) {
    if (record.schema != kControlHostBootstrapSchema ||
        record.version != kControlHostBootstrapVersion) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::UnsupportedVersion,
                        "the bootstrap schema or version is unsupported");
        return false;
    }
    const auto& peer = record.expected_broker.evidence;
    const auto endpoint = record.endpoint_path.string();
    if (endpoint.empty() || endpoint.size() > kMaximumPathBytes ||
        !record.endpoint_path.is_absolute() || peer.role != ControlPeerRole::TrustedHostBridge ||
        peer.user_id.empty() || peer.user_id.size() > kMaximumPeerFieldBytes ||
        peer.process_id <= 0 || peer.process_start_id.empty() ||
        peer.process_start_id.size() > kMaximumPeerFieldBytes || peer.executable_identity.empty() ||
        peer.executable_identity.size() > kMaximumPeerFieldBytes || peer.publisher_id.empty() ||
        peer.publisher_id.size() > kMaximumPeerFieldBytes ||
        !control_protocol_detail::valid_token(record.admission_id,
                                              control_protocol_detail::kMaximumIdBytes) ||
        !control_protocol_detail::valid_token(record.registration_id.value,
                                              control_protocol_detail::kMaximumIdBytes) ||
        record.expires_at_unix_ms <= 0) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::InvalidRecord,
                        "the bootstrap record is incomplete or outside its bounds");
        return false;
    }
    if (record.expires_at_unix_ms <= unix_ms(now)) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Expired,
                        "the bootstrap record has expired");
        return false;
    }
    return true;
}

bool read_document(ControlHostBootstrapHandle handle, std::vector<std::uint8_t>& bytes,
                   std::chrono::milliseconds timeout,
                   ControlHostBootstrapDiagnostics* diagnostics) {
    if (timeout <= 0ms) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::ReadFailed,
                        "the bootstrap read timeout is invalid");
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
#ifdef _WIN32
    const auto native = static_cast<HANDLE>(handle);
    if (native == nullptr || native == INVALID_HANDLE_VALUE ||
        GetFileType(native) != FILE_TYPE_PIPE) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Absent,
                        "the inherited bootstrap pipe is absent");
        return false;
    }
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(native, nullptr, 0, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE)
                return true;
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::ReadFailed,
                            "the inherited bootstrap pipe could not be inspected");
            return false;
        }
        if (available == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                                "the bootstrap writer did not close the record");
                return false;
            }
            std::this_thread::sleep_for(1ms);
            continue;
        }
        std::uint8_t chunk[1024];
        DWORD count = 0;
        if (!ReadFile(native, chunk, std::min<DWORD>(available, sizeof(chunk)), &count, nullptr)) {
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::ReadFailed,
                            "the inherited bootstrap pipe read failed");
            return false;
        }
        bytes.insert(bytes.end(), chunk, chunk + count);
        if (bytes.size() > kControlHostBootstrapMaximumBytes) {
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::Oversize,
                            "the bootstrap record exceeds its byte limit");
            return false;
        }
    }
#else
    if (handle < 0) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Absent,
                        "the inherited bootstrap descriptor is absent");
        return false;
    }
    struct stat descriptor_info{};
    if (::fstat(handle, &descriptor_info) != 0 || !S_ISSOCK(descriptor_info.st_mode)) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Absent,
                        "the inherited bootstrap descriptor is not a private socket");
        return false;
    }
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= 0ms) {
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                            "the bootstrap writer did not close the record");
            return false;
        }
        pollfd ready{.fd = handle, .events = POLLIN | POLLHUP, .revents = 0};
        const auto wait_ms = static_cast<int>(std::min<std::int64_t>(remaining.count(), INT_MAX));
        const auto polled = ::poll(&ready, 1, wait_ms);
        if (polled < 0 && errno == EINTR)
            continue;
        if (polled <= 0) {
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                            "the bootstrap writer did not close the record");
            return false;
        }
        std::uint8_t chunk[1024];
        const auto count = ::read(handle, chunk, sizeof(chunk));
        if (count == 0)
            return true;
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::ReadFailed,
                            "the inherited bootstrap descriptor read failed");
            return false;
        }
        bytes.insert(bytes.end(), chunk, chunk + count);
        if (bytes.size() > kControlHostBootstrapMaximumBytes) {
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::Oversize,
                            "the bootstrap record exceeds its byte limit");
            return false;
        }
    }
#endif
}

void close_handle(ControlHostBootstrapHandle handle) noexcept {
#ifdef _WIN32
    const auto native = static_cast<HANDLE>(handle);
    if (native != nullptr && native != INVALID_HANDLE_VALUE)
        CloseHandle(native);
#else
    if (handle >= 0)
        ::close(handle);
#endif
}

} // namespace

ControlHostBootstrapRecord::~ControlHostBootstrapRecord() {
    clear();
}

ControlHostBootstrapRecord::ControlHostBootstrapRecord(ControlHostBootstrapRecord&& other) noexcept
    : schema(std::move(other.schema)), version(other.version),
      endpoint_path(std::move(other.endpoint_path)),
      expected_broker(std::move(other.expected_broker)),
      admission_id(std::move(other.admission_id)),
      registration_id(std::move(other.registration_id)),
      expires_at_unix_ms(other.expires_at_unix_ms) {
    other.clear();
}

ControlHostBootstrapRecord&
ControlHostBootstrapRecord::operator=(ControlHostBootstrapRecord&& other) noexcept {
    if (this != &other) {
        clear();
        schema = std::move(other.schema);
        version = other.version;
        endpoint_path = std::move(other.endpoint_path);
        expected_broker = std::move(other.expected_broker);
        admission_id = std::move(other.admission_id);
        registration_id = std::move(other.registration_id);
        expires_at_unix_ms = other.expires_at_unix_ms;
        other.clear();
    }
    return *this;
}

void ControlHostBootstrapRecord::clear() noexcept {
    runtime::secure_zero_memory(admission_id.data(), admission_id.size());
    runtime::secure_zero_memory(registration_id.value.data(), registration_id.value.size());
    admission_id.clear();
    registration_id.value.clear();
    expires_at_unix_ms = 0;
}

ControlHostBootstrapBytes::ControlHostBootstrapBytes(std::vector<std::uint8_t> bytes)
    : bytes_(std::move(bytes)) {}

ControlHostBootstrapBytes::~ControlHostBootstrapBytes() {
    clear();
}

ControlHostBootstrapBytes::ControlHostBootstrapBytes(ControlHostBootstrapBytes&& other) noexcept
    : bytes_(std::move(other.bytes_)) {
    other.clear();
}

ControlHostBootstrapBytes&
ControlHostBootstrapBytes::operator=(ControlHostBootstrapBytes&& other) noexcept {
    if (this != &other) {
        clear();
        bytes_ = std::move(other.bytes_);
        other.clear();
    }
    return *this;
}

void ControlHostBootstrapBytes::clear() noexcept {
    runtime::secure_zero_memory(bytes_.data(), bytes_.size());
    bytes_.clear();
}

ControlHostBootstrapBytes encode_control_host_bootstrap(const ControlHostBootstrapRecord& record) {
    if (!valid_record(record, std::chrono::system_clock::now(), nullptr))
        return {};
    const auto& peer = record.expected_broker.evidence;
    auto broker = choc::value::createObject("");
    broker.addMember("executable_identity", choc::value::createString(peer.executable_identity));
    broker.addMember("process_id", choc::value::createInt64(peer.process_id));
    broker.addMember("process_start_id", choc::value::createString(peer.process_start_id));
    broker.addMember("publisher_id", choc::value::createString(peer.publisher_id));
    broker.addMember("role", choc::value::createString(role_id(peer.role)));
    broker.addMember("user_id", choc::value::createString(peer.user_id));

    auto root = choc::value::createObject("");
    root.addMember("admission_id", choc::value::createString(record.admission_id));
    root.addMember("endpoint", choc::value::createString(record.endpoint_path.string()));
    root.addMember("expected_broker", std::move(broker));
    root.addMember("expires_at_unix_ms", choc::value::createInt64(record.expires_at_unix_ms));
    root.addMember("registration_id", choc::value::createString(record.registration_id.value));
    root.addMember("schema", choc::value::createString(record.schema));
    root.addMember("version", choc::value::createInt32(static_cast<std::int32_t>(record.version)));
    const auto encoded = choc::json::toString(root, false);
    if (encoded.empty() || encoded.size() > kControlHostBootstrapMaximumBytes)
        return {};
    return ControlHostBootstrapBytes{{encoded.begin(), encoded.end()}};
}

std::optional<ControlHostBootstrapRecord>
decode_control_host_bootstrap(std::span<const std::uint8_t> bytes,
                              std::chrono::system_clock::time_point now,
                              ControlHostBootstrapDiagnostics* diagnostics) {
    if (bytes.empty()) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                        "the bootstrap record is empty");
        return std::nullopt;
    }
    if (bytes.size() > kControlHostBootstrapMaximumBytes) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Oversize,
                        "the bootstrap record exceeds its byte limit");
        return std::nullopt;
    }
    const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto parsed = control_protocol_detail::parse_bounded_control_json(
        text, kControlHostBootstrapMaximumBytes, 64);
    if (!parsed || !parsed->isObject()) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                        "the bootstrap record is not complete bounded JSON");
        return std::nullopt;
    }
    ControlProtocolDiagnostics fields;
    if (!control_protocol_detail::only_fields(*parsed,
                                              {"admission_id", "endpoint", "expected_broker",
                                               "expires_at_unix_ms", "registration_id", "schema",
                                               "version"},
                                              fields)) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::InvalidRecord, fields.explanation);
        return std::nullopt;
    }
    const auto broker = (*parsed)["expected_broker"];
    if (!broker.isObject() || !control_protocol_detail::only_fields(
                                  broker,
                                  {"executable_identity", "process_id", "process_start_id",
                                   "publisher_id", "role", "user_id"},
                                  fields)) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::InvalidRecord,
                        "the exact broker expectation is malformed");
        return std::nullopt;
    }

    ControlHostBootstrapRecord record;
    std::string endpoint;
    std::string registration;
    std::string role;
    if (!control_protocol_detail::required_string(*parsed, "schema", record.schema, 128, fields,
                                                  false) ||
        !control_protocol_detail::required_u32(*parsed, "version", record.version, fields) ||
        !control_protocol_detail::required_string(*parsed, "endpoint", endpoint, kMaximumPathBytes,
                                                  fields, false) ||
        !control_protocol_detail::required_string(*parsed, "admission_id", record.admission_id,
                                                  control_protocol_detail::kMaximumIdBytes,
                                                  fields) ||
        !control_protocol_detail::required_string(*parsed, "registration_id", registration,
                                                  control_protocol_detail::kMaximumIdBytes,
                                                  fields) ||
        !control_protocol_detail::required_i64(*parsed, "expires_at_unix_ms",
                                               record.expires_at_unix_ms, fields) ||
        !control_protocol_detail::required_string(broker, "role", role, 64, fields, false) ||
        !control_protocol_detail::required_string(broker, "user_id",
                                                  record.expected_broker.evidence.user_id,
                                                  kMaximumPeerFieldBytes, fields, false) ||
        !control_protocol_detail::required_i64(
            broker, "process_id", record.expected_broker.evidence.process_id, fields) ||
        !control_protocol_detail::required_string(broker, "process_start_id",
                                                  record.expected_broker.evidence.process_start_id,
                                                  kMaximumPeerFieldBytes, fields, false) ||
        !control_protocol_detail::required_string(
            broker, "executable_identity", record.expected_broker.evidence.executable_identity,
            kMaximumPeerFieldBytes, fields, false) ||
        !control_protocol_detail::required_string(broker, "publisher_id",
                                                  record.expected_broker.evidence.publisher_id,
                                                  kMaximumPeerFieldBytes, fields, false)) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::InvalidRecord, fields.explanation);
        return std::nullopt;
    }
    const auto parsed_role = parse_role(role);
    if (!parsed_role) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::InvalidRecord,
                        "the broker role is unsupported");
        return std::nullopt;
    }
    record.endpoint_path = std::filesystem::path(endpoint);
    record.registration_id = ControlRegistrationId{std::move(registration)};
    record.expected_broker.evidence.role = *parsed_role;
    if (!valid_record(record, now, diagnostics))
        return std::nullopt;
    set_diagnostics(diagnostics, ControlHostBootstrapStatus::Accepted, {});
    return record;
}

std::optional<ControlHostBootstrapRecord>
read_control_host_bootstrap(ControlHostBootstrapHandle handle, std::chrono::milliseconds timeout,
                            std::chrono::system_clock::time_point now,
                            ControlHostBootstrapDiagnostics* diagnostics) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kControlHostBootstrapMaximumBytes);
    const bool read = read_document(handle, bytes, timeout, diagnostics);
    close_handle(handle);
    auto record = read ? decode_control_host_bootstrap(bytes, now, diagnostics) : std::nullopt;
    runtime::secure_zero_memory(bytes.data(), bytes.size());
    return record;
}

ControlHostBootstrapHandle inherited_control_host_bootstrap_handle() noexcept {
#ifdef _WIN32
    const auto handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
        GetFileType(handle) != FILE_TYPE_PIPE)
        return nullptr;
#else
    struct stat descriptor_info{};
    if (::fstat(STDIN_FILENO, &descriptor_info) != 0 || !S_ISSOCK(descriptor_info.st_mode))
        return -1;
#endif
    static std::atomic_bool claimed = false;
    if (claimed.exchange(true, std::memory_order_acq_rel)) {
#ifdef _WIN32
        return nullptr;
#else
        return -1;
#endif
    }
#ifdef _WIN32
    return handle;
#else
    return STDIN_FILENO;
#endif
}

} // namespace pulp::inspect
