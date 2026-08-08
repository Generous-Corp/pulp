#include <pulp/inspect/control_host_bootstrap.hpp>

#include "control_protocol_internal.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
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

void wipe_string_storage(std::string& value) noexcept {
    // Credentials are public fields for legacy API compatibility, so callers
    // can shorten them without first wiping the old suffix. Expand within the
    // retained allocation before zeroing to cover that suffix as well.
    try {
        value.resize(value.capacity());
    } catch (...) {
        // A conforming implementation does not allocate when resizing within
        // capacity. Preserve noexcept cleanup and still wipe the live value if
        // an implementation reports a failure.
    }
    runtime::secure_zero_memory(value.data(), value.size());
    value.clear();
}

void wipe_byte_storage(std::vector<std::uint8_t>& value) noexcept {
    try {
        value.resize(value.capacity());
    } catch (...) {
    }
    runtime::secure_zero_memory(value.data(), value.size());
    value.clear();
}

class ByteStorageWiper {
  public:
    explicit ByteStorageWiper(std::vector<std::uint8_t>& value) : value_(value) {}
    ~ByteStorageWiper() {
        wipe_byte_storage(value_);
    }
    ByteStorageWiper(const ByteStorageWiper&) = delete;
    ByteStorageWiper& operator=(const ByteStorageWiper&) = delete;

  private:
    std::vector<std::uint8_t>& value_;
};

using namespace std::chrono_literals;

constexpr std::size_t kMaximumPathBytes = 4096;
constexpr std::size_t kMaximumPeerFieldBytes = 4096;

void set_diagnostics(ControlHostBootstrapDiagnostics* diagnostics,
                     ControlHostBootstrapStatus status, std::string explanation) {
    if (diagnostics)
        *diagnostics = {.status = status, .explanation = std::move(explanation)};
}

class SecureJsonWriter {
  public:
    SecureJsonWriter() {
        bytes_.reserve(kControlHostBootstrapMaximumBytes);
    }
    ~SecureJsonWriter() {
        wipe_byte_storage(bytes_);
    }
    SecureJsonWriter(const SecureJsonWriter&) = delete;
    SecureJsonWriter& operator=(const SecureJsonWriter&) = delete;

    bool append(std::string_view value) {
        if (value.size() > kControlHostBootstrapMaximumBytes - bytes_.size())
            return false;
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }

    bool append_string(std::string_view value) {
        if (!append("\""))
            return false;
        static constexpr char hex[] = "0123456789abcdef";
        for (const auto c : value) {
            switch (static_cast<unsigned char>(c)) {
            case '"':
                if (!append("\\\""))
                    return false;
                break;
            case '\\':
                if (!append("\\\\"))
                    return false;
                break;
            case '\b':
                if (!append("\\b"))
                    return false;
                break;
            case '\f':
                if (!append("\\f"))
                    return false;
                break;
            case '\n':
                if (!append("\\n"))
                    return false;
                break;
            case '\r':
                if (!append("\\r"))
                    return false;
                break;
            case '\t':
                if (!append("\\t"))
                    return false;
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    const std::array escaped{'\\',
                                             'u',
                                             '0',
                                             '0',
                                             hex[(static_cast<unsigned char>(c) >> 4u) & 0x0fu],
                                             hex[static_cast<unsigned char>(c) & 0x0fu]};
                    if (!append(std::string_view(escaped.data(), escaped.size())))
                        return false;
                } else if (!append(std::string_view(&c, 1))) {
                    return false;
                }
            }
        }
        return append("\"");
    }

    template <typename Integer> bool append_integer(Integer value) {
        std::array<char, 32> text{};
        const auto [end, error] = std::to_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc{} &&
               append(std::string_view(text.data(), static_cast<std::size_t>(end - text.data())));
    }

    std::vector<std::uint8_t> take() {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

void skip_json_whitespace(std::span<std::uint8_t> json, std::size_t& cursor) {
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' ||
                                    json[cursor] == '\r' || json[cursor] == '\n'))
        ++cursor;
}

int hex_digit(std::uint8_t value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return 10 + value - 'a';
    if (value >= 'A' && value <= 'F')
        return 10 + value - 'A';
    return -1;
}

bool parse_ascii_json_string(std::span<std::uint8_t> json, std::size_t& cursor, std::string& out,
                             bool redact) {
    if (cursor >= json.size() || json[cursor] != '"')
        return false;
    const auto content_begin = ++cursor;
    out.clear();
    while (cursor < json.size()) {
        auto value = json[cursor++];
        if (value == '"') {
            if (redact)
                std::fill(json.begin() + static_cast<std::ptrdiff_t>(content_begin),
                          json.begin() + static_cast<std::ptrdiff_t>(cursor - 1), 'x');
            return true;
        }
        if (value != '\\') {
            if (value > 0x7e)
                return false;
            out.push_back(static_cast<char>(value));
            continue;
        }
        if (cursor >= json.size())
            return false;
        value = json[cursor++];
        switch (value) {
        case '"':
        case '\\':
        case '/':
            out.push_back(static_cast<char>(value));
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            if (json.size() - cursor < 4)
                return false;
            unsigned decoded = 0;
            for (unsigned index = 0; index < 4; ++index) {
                const auto digit = hex_digit(json[cursor++]);
                if (digit < 0)
                    return false;
                decoded = (decoded << 4u) | static_cast<unsigned>(digit);
            }
            if (decoded > 0x7e)
                return false;
            out.push_back(static_cast<char>(decoded));
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

bool skip_json_value(std::span<std::uint8_t> json, std::size_t& cursor) {
    std::size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; cursor < json.size(); ++cursor) {
        const auto value = json[cursor];
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (value == '\\')
                escaped = true;
            else if (value == '"')
                in_string = false;
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '{' || value == '[') {
            ++depth;
        } else if (value == '}' || value == ']') {
            if (depth == 0)
                return true;
            --depth;
        } else if (value == ',' && depth == 0) {
            return true;
        }
    }
    return false;
}

bool extract_and_redact_credentials(std::span<std::uint8_t> json,
                                    ControlHostBootstrapRecord& record,
                                    ControlProtocolDiagnostics& diagnostics) {
    record.admission_id.reserve(control_protocol_detail::kMaximumIdBytes);
    record.registration_id.value.reserve(control_protocol_detail::kMaximumIdBytes);
    record.enrollment_id.reserve(control_protocol_detail::kMaximumIdBytes);
    bool found_admission = false;
    bool found_registration = false;
    bool found_enrollment = false;
    std::size_t cursor = 0;
    skip_json_whitespace(json, cursor);
    if (cursor >= json.size() || json[cursor++] != '{')
        return false;
    for (;;) {
        skip_json_whitespace(json, cursor);
        if (cursor < json.size() && json[cursor] == '}')
            break;
        std::string name;
        if (!parse_ascii_json_string(json, cursor, name, false))
            return false;
        skip_json_whitespace(json, cursor);
        if (cursor >= json.size() || json[cursor++] != ':')
            return false;
        skip_json_whitespace(json, cursor);

        std::string* credential = nullptr;
        bool* found = nullptr;
        if (name == "admission_id") {
            credential = &record.admission_id;
            found = &found_admission;
        } else if (name == "registration_id") {
            credential = &record.registration_id.value;
            found = &found_registration;
        } else if (name == "enrollment_id") {
            credential = &record.enrollment_id;
            found = &found_enrollment;
        }
        if (credential) {
            if (*found)
                return false;
            if (!parse_ascii_json_string(json, cursor, *credential, true)) {
                diagnostics = {ControlProtocolError::InvalidType,
                               "credential fields must be strings"};
                return false;
            }
            *found = true;
            if (!credential->empty() &&
                !control_protocol_detail::valid_token(*credential,
                                                      control_protocol_detail::kMaximumIdBytes)) {
                diagnostics = {ControlProtocolError::InvalidValue, "a credential field is invalid"};
                return false;
            }
        } else if (!skip_json_value(json, cursor)) {
            return false;
        }
        skip_json_whitespace(json, cursor);
        if (cursor < json.size() && json[cursor] == ',') {
            ++cursor;
            continue;
        }
        if (cursor < json.size() && json[cursor] == '}')
            break;
        return false;
    }
    if (!found_admission || !found_registration) {
        diagnostics = {ControlProtocolError::MissingField, "missing required credential field"};
        return false;
    }
    if (!found_enrollment)
        record.enrollment_id.clear();
    return true;
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

bool valid_credentials(const ControlHostBootstrapRecord& record) {
    const auto valid_id = [](std::string_view value) {
        return control_protocol_detail::valid_token(value,
                                                    control_protocol_detail::kMaximumIdBytes);
    };
    const bool preissued = record.enrollment_id.empty() && valid_id(record.admission_id) &&
                           valid_id(record.registration_id.value);
    const bool enrollment = record.admission_id.empty() && record.registration_id.value.empty() &&
                            valid_id(record.enrollment_id);
    return preissued || enrollment;
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
        peer.publisher_id.size() > kMaximumPeerFieldBytes || !valid_credentials(record) ||
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
      enrollment_id(std::move(other.enrollment_id)), expires_at_unix_ms(other.expires_at_unix_ms) {
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
        enrollment_id = std::move(other.enrollment_id);
        expires_at_unix_ms = other.expires_at_unix_ms;
        other.clear();
    }
    return *this;
}

void ControlHostBootstrapRecord::clear() noexcept {
    wipe_string_storage(admission_id);
    wipe_string_storage(registration_id.value);
    wipe_string_storage(enrollment_id);
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
    wipe_byte_storage(bytes_);
}

ControlHostBootstrapBytes encode_control_host_bootstrap(const ControlHostBootstrapRecord& record) {
    if (!valid_record(record, std::chrono::system_clock::now(), nullptr))
        return {};
    const auto& peer = record.expected_broker.evidence;
    SecureJsonWriter encoded;
    const auto endpoint = record.endpoint_path.string();
    bool complete = encoded.append("{ \"admission_id\": ") &&
                    encoded.append_string(record.admission_id) &&
                    encoded.append(", \"endpoint\": ") && encoded.append_string(endpoint);
    if (complete && !record.enrollment_id.empty())
        complete =
            encoded.append(", \"enrollment_id\": ") && encoded.append_string(record.enrollment_id);
    complete =
        complete && encoded.append(", \"expected_broker\": { \"executable_identity\": ") &&
        encoded.append_string(peer.executable_identity) && encoded.append(", \"process_id\": ") &&
        encoded.append_integer(peer.process_id) && encoded.append(", \"process_start_id\": ") &&
        encoded.append_string(peer.process_start_id) && encoded.append(", \"publisher_id\": ") &&
        encoded.append_string(peer.publisher_id) && encoded.append(", \"role\": ") &&
        encoded.append_string(role_id(peer.role)) && encoded.append(", \"user_id\": ") &&
        encoded.append_string(peer.user_id) && encoded.append(" }, \"expires_at_unix_ms\": ") &&
        encoded.append_integer(record.expires_at_unix_ms) &&
        encoded.append(", \"registration_id\": ") &&
        encoded.append_string(record.registration_id.value) && encoded.append(", \"schema\": ") &&
        encoded.append_string(record.schema) && encoded.append(", \"version\": ") &&
        encoded.append_integer(record.version) && encoded.append(" }");
    auto bytes = encoded.take();
    if (!complete || bytes.empty()) {
        wipe_byte_storage(bytes);
        return {};
    }
    return ControlHostBootstrapBytes{std::move(bytes)};
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
    if (!control_protocol_detail::valid_control_json_bytes(text, kControlHostBootstrapMaximumBytes,
                                                           64)) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                        "the bootstrap record is not complete bounded JSON");
        return std::nullopt;
    }
    std::vector<std::uint8_t> redacted(bytes.size() + 1);
    ByteStorageWiper wipe_redacted(redacted);
    std::ranges::copy(bytes, redacted.begin());
    ControlHostBootstrapRecord record;
    ControlProtocolDiagnostics fields;
    if (!extract_and_redact_credentials(std::span<std::uint8_t>(redacted.data(), bytes.size()),
                                        record, fields)) {
        if (fields.explanation.empty())
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                            "the bootstrap record is not complete bounded JSON");
        else
            set_diagnostics(diagnostics, ControlHostBootstrapStatus::InvalidRecord,
                            fields.explanation);
        return std::nullopt;
    }
    std::optional<choc::value::Value> parsed;
    try {
        parsed = choc::json::parseValue(
            std::string_view(reinterpret_cast<const char*>(redacted.data()), bytes.size()));
    } catch (...) {
    }
    std::size_t remaining_nodes = 64;
    if (!parsed || !control_protocol_detail::bounded_json_shape(*parsed, 0, remaining_nodes)) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                        "the bootstrap record is not complete bounded JSON");
        return std::nullopt;
    }
    if (!parsed->isObject()) {
        set_diagnostics(diagnostics, ControlHostBootstrapStatus::Truncated,
                        "the bootstrap record is not complete bounded JSON");
        return std::nullopt;
    }
    if (!control_protocol_detail::only_fields(*parsed,
                                              {"admission_id", "endpoint", "expected_broker",
                                               "enrollment_id", "expires_at_unix_ms",
                                               "registration_id", "schema", "version"},
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

    std::string endpoint;
    std::string role;
    if (!control_protocol_detail::required_string(*parsed, "schema", record.schema, 128, fields,
                                                  false) ||
        !control_protocol_detail::required_u32(*parsed, "version", record.version, fields) ||
        !control_protocol_detail::required_string(*parsed, "endpoint", endpoint, kMaximumPathBytes,
                                                  fields, false) ||
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
