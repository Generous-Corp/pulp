#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_host_bootstrap.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/base64.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <random>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE
#error "PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE must name the bootstrap child fixture"
#endif

using namespace std::chrono_literals;
using namespace pulp::inspect;

namespace {

ControlPeerEvidence synthetic_broker() {
    return {.role = ControlPeerRole::TrustedHostBridge,
            .user_id = "user-1",
            .process_id = 42,
            .process_start_id = "start-1",
            .executable_identity = "signed:broker",
            .publisher_id = "publisher-1"};
}

ControlHostBootstrapRecord make_record(
    std::filesystem::path endpoint = "/tmp/pulp-control-bootstrap.sock",
    std::chrono::system_clock::time_point expiry = std::chrono::system_clock::now() + 1min) {
    ControlHostBootstrapRecord record;
    record.endpoint_path = std::move(endpoint);
    record.expected_broker = {.evidence = synthetic_broker()};
    record.admission_id = "admission-1";
    record.registration_id = ControlRegistrationId{"registration-1"};
    record.expires_at_unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(expiry.time_since_epoch()).count();
    return record;
}

ControlHostBootstrapRecord make_enrollment_record() {
    auto record = make_record();
    record.admission_id.clear();
    record.registration_id.value.clear();
    record.enrollment_id = "ZW5yb2xsbWVudC0x+/==";
    return record;
}

std::string encoded_text(const ControlHostBootstrapBytes& encoded) {
    return {reinterpret_cast<const char*>(encoded.bytes().data()), encoded.bytes().size()};
}

std::vector<std::uint8_t> replace_once(const ControlHostBootstrapBytes& encoded,
                                       std::string_view before, std::string_view after) {
    auto text = encoded_text(encoded);
    const auto at = text.find(before);
    REQUIRE(at != std::string::npos);
    text.replace(at, before.size(), after);
    return {text.begin(), text.end()};
}

void replace_version(std::vector<std::uint8_t>& bytes) {
    std::string text(bytes.begin(), bytes.end());
    auto at = text.find("\"version\": 1");
    std::size_t offset = 11;
    if (at == std::string::npos) {
        at = text.find("\"version\":1");
        offset = 10;
    }
    REQUIRE(at != std::string::npos);
    text[at + offset] = '2';
    bytes.assign(text.begin(), text.end());
}

#ifndef _WIN32
std::optional<ControlHostBootstrapRecord> read_from_socket(std::span<const std::uint8_t> bytes,
                                                           ControlHostBootstrapDiagnostics& diag,
                                                           int* duplicate = nullptr) {
    int pair[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    if (duplicate)
        *duplicate = ::dup(pair[1]);
    REQUIRE(::write(pair[0], bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
    ::close(pair[0]);
    return read_control_host_bootstrap(pair[1], 500ms, std::chrono::system_clock::now(), &diag);
}
#endif

} // namespace

TEST_CASE("control host bootstrap codec rejects malformed and stale records",
          "[inspect][control][host][bootstrap]") {
    auto record = make_record();
    auto encoded = encode_control_host_bootstrap(record);
    REQUIRE_FALSE(encoded.empty());

    ControlHostBootstrapDiagnostics diagnostics;
    auto decoded = decode_control_host_bootstrap(encoded.bytes(), std::chrono::system_clock::now(),
                                                 &diagnostics);
    REQUIRE(decoded);
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Accepted);
    CHECK(decoded->registration_id.value == "registration-1");

    auto truncated = std::vector<std::uint8_t>(encoded.bytes().begin(), encoded.bytes().end());
    truncated.pop_back();
    CHECK_FALSE(
        decode_control_host_bootstrap(truncated, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Truncated);

    std::vector<std::uint8_t> oversize(kControlHostBootstrapMaximumBytes + 1, 'x');
    CHECK_FALSE(
        decode_control_host_bootstrap(oversize, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Oversize);

    auto unknown = std::vector<std::uint8_t>(encoded.bytes().begin(), encoded.bytes().end());
    replace_version(unknown);
    CHECK_FALSE(
        decode_control_host_bootstrap(unknown, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::UnsupportedVersion);

    CHECK_FALSE(decode_control_host_bootstrap(
        encoded.bytes(), std::chrono::system_clock::now() + 2min, &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Expired);
}

TEST_CASE("control host bootstrap codec accepts exactly one credential mode",
          "[inspect][control][host][bootstrap][security]") {
    const auto now = std::chrono::system_clock::now();
    ControlHostBootstrapDiagnostics diagnostics;

    auto preissued = make_record();
    const auto encoded_preissued = encode_control_host_bootstrap(preissued);
    REQUIRE_FALSE(encoded_preissued.empty());
    CHECK(encoded_text(encoded_preissued).find(R"("enrollment_id": "")") != std::string::npos);
    auto decoded_preissued =
        decode_control_host_bootstrap(encoded_preissued.bytes(), now, &diagnostics);
    REQUIRE(decoded_preissued);
    CHECK(decoded_preissued->admission_id == "admission-1");
    CHECK(decoded_preissued->registration_id.value == "registration-1");
    CHECK(decoded_preissued->enrollment_id.empty());

    auto enrollment = make_enrollment_record();
    const auto encoded_enrollment = encode_control_host_bootstrap(enrollment);
    REQUIRE_FALSE(encoded_enrollment.empty());
    auto decoded_enrollment =
        decode_control_host_bootstrap(encoded_enrollment.bytes(), now, &diagnostics);
    REQUIRE(decoded_enrollment);
    CHECK(decoded_enrollment->admission_id.empty());
    CHECK(decoded_enrollment->registration_id.value.empty());
    CHECK(decoded_enrollment->enrollment_id == "ZW5yb2xsbWVudC0x+/==");

    const auto legacy = replace_once(encoded_preissued, R"(, "enrollment_id": "")", "");
    auto decoded_legacy = decode_control_host_bootstrap(legacy, now, &diagnostics);
    REQUIRE(decoded_legacy);
    CHECK(decoded_legacy->admission_id == "admission-1");
    CHECK(decoded_legacy->registration_id.value == "registration-1");
    CHECK(decoded_legacy->enrollment_id.empty());

    const auto carrier = pulp::runtime::base64_encode(encoded_enrollment.bytes().data(),
                                                      encoded_enrollment.bytes().size());
    const auto unpacked = pulp::runtime::base64_decode(carrier);
    REQUIRE(unpacked);
    auto decoded_carrier = decode_control_host_bootstrap(*unpacked, now, &diagnostics);
    REQUIRE(decoded_carrier);
    CHECK(decoded_carrier->enrollment_id == enrollment.enrollment_id);

    auto escaped = make_enrollment_record();
    escaped.enrollment_id = "enrollment-\"quoted\"-\\escaped";
    const auto encoded_escaped = encode_control_host_bootstrap(escaped);
    REQUIRE_FALSE(encoded_escaped.empty());
    CHECK(encoded_text(encoded_escaped).find("enrollment-\\\"quoted\\\"-\\\\escaped") !=
          std::string::npos);
    auto decoded_escaped =
        decode_control_host_bootstrap(encoded_escaped.bytes(), now, &diagnostics);
    REQUIRE(decoded_escaped);
    CHECK(decoded_escaped->enrollment_id == escaped.enrollment_id);
}

TEST_CASE("control host bootstrap codec rejects mixed and malformed credential modes",
          "[inspect][control][host][bootstrap][security]") {
    const auto now = std::chrono::system_clock::now();
    ControlHostBootstrapDiagnostics diagnostics;

    auto expect_encode_rejected = [&](ControlHostBootstrapRecord record) {
        CHECK(encode_control_host_bootstrap(record).empty());
    };

    auto neither = make_record();
    neither.admission_id.clear();
    neither.registration_id.value.clear();
    expect_encode_rejected(std::move(neither));

    auto admission_only = make_record();
    admission_only.registration_id.value.clear();
    expect_encode_rejected(std::move(admission_only));

    auto registration_only = make_record();
    registration_only.admission_id.clear();
    expect_encode_rejected(std::move(registration_only));

    auto all = make_record();
    all.enrollment_id = "enrollment-secret";
    expect_encode_rejected(std::move(all));

    auto enrollment_and_admission = make_enrollment_record();
    enrollment_and_admission.admission_id = "admission-secret";
    expect_encode_rejected(std::move(enrollment_and_admission));

    auto oversized = make_enrollment_record();
    oversized.enrollment_id.assign(129, 'x');
    expect_encode_rejected(std::move(oversized));

    auto enrollment = make_enrollment_record();
    const auto encoded = encode_control_host_bootstrap(enrollment);
    REQUIRE_FALSE(encoded.empty());
    const auto duplicate =
        replace_once(encoded, R"("enrollment_id": "ZW5yb2xsbWVudC0x+/==")",
                     R"("enrollment_id": "duplicate", "enrollment_id": "ZW5yb2xsbWVudC0x+/==")");
    CHECK_FALSE(decode_control_host_bootstrap(duplicate, now, &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Truncated);
    CHECK(diagnostics.explanation.find(enrollment.enrollment_id) == std::string::npos);

    const auto unknown =
        replace_once(encoded, R"("version": 1)", R"("unknown": "value", "version": 1)");
    CHECK_FALSE(decode_control_host_bootstrap(unknown, now, &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::InvalidRecord);

    const auto oversized_enrollment =
        replace_once(encoded, R"("enrollment_id": "ZW5yb2xsbWVudC0x+/==")",
                     "\"enrollment_id\": \"" + std::string(129, 'x') + "\"");
    CHECK_FALSE(decode_control_host_bootstrap(oversized_enrollment, now, &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::InvalidRecord);

    const auto mixed =
        replace_once(encoded, R"("admission_id": "")", R"("admission_id": "admission-secret")");
    CHECK_FALSE(decode_control_host_bootstrap(mixed, now, &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::InvalidRecord);
    CHECK(diagnostics.explanation.find("admission-secret") == std::string::npos);
    CHECK(diagnostics.explanation.find(enrollment.enrollment_id) == std::string::npos);

    CHECK_FALSE(decode_control_host_bootstrap(encoded.bytes(), now + 2min, &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Expired);
}

TEST_CASE("control host bootstrap credential storage clears every mode",
          "[inspect][control][host][bootstrap][security]") {
    auto preissued = make_record();
    preissued.clear();
    CHECK(preissued.admission_id.empty());
    CHECK(preissued.registration_id.value.empty());
    CHECK(preissued.enrollment_id.empty());

    auto enrollment = make_enrollment_record();
    auto moved = std::move(enrollment);
    CHECK(enrollment.admission_id.empty());
    CHECK(enrollment.registration_id.value.empty());
    CHECK(enrollment.enrollment_id.empty());
    moved.clear();
    CHECK(moved.admission_id.empty());
    CHECK(moved.registration_id.value.empty());
    CHECK(moved.enrollment_id.empty());
    CHECK(moved.expires_at_unix_ms == 0);
}

TEST_CASE("control host bootstrap decoder remains closed under deterministic mutation fuzz",
          "[inspect][control][host][bootstrap][fuzz]") {
    auto preissued = make_record();
    auto enrollment = make_enrollment_record();
    const auto preissued_bytes = encode_control_host_bootstrap(preissued);
    const auto enrollment_bytes = encode_control_host_bootstrap(enrollment);
    REQUIRE_FALSE(preissued_bytes.empty());
    REQUIRE_FALSE(enrollment_bytes.empty());
    const std::array seeds{encoded_text(preissued_bytes), encoded_text(enrollment_bytes)};
    std::mt19937_64 random(0x7264u);
    const auto now = std::chrono::system_clock::now();

    for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
        auto bytes = seeds[iteration % seeds.size()];
        const auto mutations = 1 + random() % 8;
        for (std::size_t mutation = 0; mutation < mutations; ++mutation) {
            if (bytes.empty() || (random() & 3u) == 0)
                bytes.insert(bytes.begin() +
                                 static_cast<std::ptrdiff_t>(random() % (bytes.size() + 1)),
                             static_cast<char>(random() & 0xffu));
            else if ((random() & 1u) == 0)
                bytes[random() % bytes.size()] = static_cast<char>(random() & 0xffu);
            else
                bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(random() % bytes.size()));
        }
        const auto decoded = decode_control_host_bootstrap(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                          bytes.size()),
            now);
        if (!decoded)
            continue;
        const auto canonical = encode_control_host_bootstrap(*decoded);
        REQUIRE_FALSE(canonical.empty());
        auto repeated = decode_control_host_bootstrap(canonical.bytes(), now);
        REQUIRE(repeated);
        CHECK(repeated->admission_id == decoded->admission_id);
        CHECK(repeated->registration_id == decoded->registration_id);
        CHECK(repeated->enrollment_id == decoded->enrollment_id);
    }
}

TEST_CASE("control host bootstrap handle is consumed exactly once",
          "[inspect][control][host][bootstrap][security]") {
#ifndef _WIN32
    auto record = make_record();
    const auto encoded = encode_control_host_bootstrap(record);
    ControlHostBootstrapDiagnostics diagnostics;
    int duplicate = -1;
    auto first = read_from_socket(encoded.bytes(), diagnostics, &duplicate);
    REQUIRE(first);
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Accepted);
    auto second = read_control_host_bootstrap(duplicate, 100ms, std::chrono::system_clock::now(),
                                              &diagnostics);
    CHECK_FALSE(second);
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Truncated);

    CHECK_FALSE(
        read_control_host_bootstrap(-1, 100ms, std::chrono::system_clock::now(), &diagnostics));
    CHECK(diagnostics.status == ControlHostBootstrapStatus::Absent);

    auto enrollment = make_enrollment_record();
    const auto enrollment_encoded = encode_control_host_bootstrap(enrollment);
    int enrollment_duplicate = -1;
    auto enrollment_first =
        read_from_socket(enrollment_encoded.bytes(), diagnostics, &enrollment_duplicate);
    REQUIRE(enrollment_first);
    CHECK(enrollment_first->enrollment_id == enrollment.enrollment_id);
    CHECK_FALSE(read_control_host_bootstrap(enrollment_duplicate, 100ms,
                                            std::chrono::system_clock::now(), &diagnostics));
#else
    SUCCEED("the inherited HANDLE path is covered by the process fixture");
#endif
}

TEST_CASE("control host bootstrap inherited input leaks no unrelated descriptor",
          "[inspect][control][host][bootstrap][process][security]") {
    auto record = make_record();
    SECTION("preissued credentials") {}
    SECTION("enrollment credential") {
        record = make_enrollment_record();
    }
    pulp::platform::ProcessOptions options;
    options.timeout_ms = 5000;
    options.max_output_bytes = 4u * 1024u * 1024u;

#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_handle = INVALID_HANDLE_VALUE;
    HANDLE write_handle = INVALID_HANDLE_VALUE;
    REQUIRE(CreatePipe(&read_handle, &write_handle, &attributes, 0));
    const auto unrelated = std::to_string(reinterpret_cast<std::uintptr_t>(read_handle));
#else
    int sentinel[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sentinel) == 0);
    const auto flags = ::fcntl(sentinel[0], F_GETFD);
    REQUIRE(flags >= 0);
    REQUIRE(::fcntl(sentinel[0], F_SETFD, flags & ~FD_CLOEXEC) == 0);
    const auto unrelated = std::to_string(sentinel[0]);
#endif

    auto encoded = encode_control_host_bootstrap(record);
    pulp::platform::ChildProcess child;
    REQUIRE(child.start_with_standard_input(PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE,
                                            {"--unrelated-handle", unrelated}, encoded.bytes(),
                                            options));
    const auto result = child.wait();
    CHECK(result.exit_code == 0);
    const auto expected = record.enrollment_id.empty() ? "decoded-preissued" : "decoded-enrollment";
    CHECK(result.stdout_output.find(expected) != std::string::npos);
    const auto absent_from_output = [&](std::string_view credential) {
        return credential.empty() || result.stdout_output.find(credential) == std::string::npos;
    };
    CHECK(absent_from_output(record.admission_id));
    CHECK(absent_from_output(record.registration_id.value));
    CHECK(absent_from_output(record.enrollment_id));

#ifdef _WIN32
    CloseHandle(read_handle);
    CloseHandle(write_handle);
#else
    ::close(sentinel[0]);
    ::close(sentinel[1]);
#endif
}

TEST_CASE("inherited input delivery is bounded when the child does not read",
          "[inspect][control][host][bootstrap][process][security]") {
    std::vector<std::uint8_t> input(64u * 1024u * 1024u, 'x');
    pulp::platform::ProcessOptions options;
    options.standard_input_timeout_ms = 50;
    const auto started_at = std::chrono::steady_clock::now();
    pulp::platform::ChildProcess child;
    CHECK_FALSE(child.start_with_standard_input(PULP_CONTROL_HOST_BOOTSTRAP_FIXTURE, {"--stall"},
                                                input, options));
    CHECK(std::chrono::steady_clock::now() - started_at < 1500ms);
}
