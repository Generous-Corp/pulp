#include <catch2/catch_test_macros.hpp>
#include <choc/memory/choc_Endianness.h>
#include <pulp/format/plugin_state_io.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::uint8_t kEnvelopeMagic[4] = {'P', 'L', 'S', 'T'};

std::uint32_t crc32_simple(const std::uint8_t* data, std::size_t len) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            const std::uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    std::uint8_t out[4];
    choc::memory::writeLittleEndian(out, value);
    bytes.insert(bytes.end(), out, out + sizeof(out));
}

std::vector<std::uint8_t> make_envelope(std::span<const std::uint8_t> store_blob,
                                        std::span<const std::uint8_t> plugin_blob = {},
                                        std::uint32_t version = 1) {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), kEnvelopeMagic, kEnvelopeMagic + 4);
    append_u32(bytes, version);
    append_u32(bytes, static_cast<std::uint32_t>(store_blob.size()));
    append_u32(bytes, static_cast<std::uint32_t>(plugin_blob.size()));
    bytes.insert(bytes.end(), store_blob.begin(), store_blob.end());
    bytes.insert(bytes.end(), plugin_blob.begin(), plugin_blob.end());
    append_u32(bytes, crc32_simple(bytes.data(), bytes.size()));
    return bytes;
}

std::vector<std::uint8_t> legacy_store_blob() {
    return {'P', 'U', 'L', 'P', 1, 2, 3, 4};
}

} // namespace

TEST_CASE("plugin_state_io restore diagnostics identify legacy and envelope shapes",
          "[format][plugin-state][restore-diagnostics]") {
    namespace io = pulp::format::plugin_state_io;

    io::RestoreDiagnosticPolicy policy;
    policy.large_state_threshold_bytes = 4;

    auto legacy = legacy_store_blob();
    auto legacy_report = io::inspect_restore_blob(legacy, policy);
    REQUIRE(legacy_report.ok());
    REQUIRE(legacy_report.format == io::RestoreBlobFormat::legacy_state_store);
    REQUIRE(legacy_report.total_bytes == legacy.size());
    REQUIRE(legacy_report.store_bytes == legacy.size());
    REQUIRE(legacy_report.plugin_bytes == 0);
    REQUIRE_FALSE(legacy_report.requires_plugin_restore);
    REQUIRE_FALSE(legacy_report.may_require_resource_relink);
    REQUIRE(legacy_report.large_state);
    REQUIRE(std::string(io::restore_diagnostic_status_name(legacy_report.status)) == "ok");

    const std::vector<std::uint8_t> plugin = {'r', 'e', 's'};
    auto envelope = make_envelope(legacy, plugin);
    auto envelope_report = io::inspect_restore_blob(envelope, policy);
    REQUIRE(envelope_report.ok());
    REQUIRE(envelope_report.format == io::RestoreBlobFormat::envelope);
    REQUIRE(envelope_report.envelope_version == io::current_envelope_version());
    REQUIRE(envelope_report.total_bytes == envelope.size());
    REQUIRE(envelope_report.store_bytes == legacy.size());
    REQUIRE(envelope_report.plugin_bytes == plugin.size());
    REQUIRE_FALSE(envelope_report.requires_envelope_migration);
    REQUIRE(envelope_report.requires_plugin_restore);
    REQUIRE(envelope_report.may_require_resource_relink);
    REQUIRE(envelope_report.large_state);
}

TEST_CASE("plugin_state_io restore diagnostics report size budgets",
          "[format][plugin-state][restore-diagnostics]") {
    namespace io = pulp::format::plugin_state_io;

    auto store = legacy_store_blob();
    const std::vector<std::uint8_t> plugin = {'p', 'l', 'u', 'g', 'i', 'n'};
    auto envelope = make_envelope(store, plugin);

    SECTION("total budget") {
        io::RestoreDiagnosticPolicy policy;
        policy.max_total_bytes = envelope.size() - 1;
        auto report = io::inspect_restore_blob(envelope, policy);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::total_size_budget_exceeded);
    }

    SECTION("store budget") {
        io::RestoreDiagnosticPolicy policy;
        policy.max_store_bytes = store.size() - 1;
        auto report = io::inspect_restore_blob(envelope, policy);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::store_size_budget_exceeded);
    }

    SECTION("plugin budget") {
        io::RestoreDiagnosticPolicy policy;
        policy.max_plugin_bytes = plugin.size() - 1;
        auto report = io::inspect_restore_blob(envelope, policy);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::plugin_size_budget_exceeded);
    }
}

TEST_CASE("plugin_state_io restore diagnostics classify malformed blobs",
          "[format][plugin-state][restore-diagnostics]") {
    namespace io = pulp::format::plugin_state_io;

    SECTION("empty blob") {
        auto report = io::inspect_restore_blob({});
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::empty_blob);
    }

    SECTION("unknown format") {
        const std::array<std::uint8_t, 4> blob = {'N', 'O', 'P', 'E'};
        auto report = io::inspect_restore_blob(blob);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::unknown_format);
    }

    SECTION("total budget wins before format parsing") {
        const std::array<std::uint8_t, 4> blob = {'N', 'O', 'P', 'E'};
        io::RestoreDiagnosticPolicy policy;
        policy.max_total_bytes = 3;
        auto report = io::inspect_restore_blob(blob, policy);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::total_size_budget_exceeded);
    }

    SECTION("truncated envelope") {
        const std::array<std::uint8_t, 4> blob = {'P', 'L', 'S', 'T'};
        auto report = io::inspect_restore_blob(blob);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::truncated_envelope);
        REQUIRE(report.format == io::RestoreBlobFormat::envelope);
    }

    SECTION("unsupported envelope version") {
        auto store = legacy_store_blob();
        auto blob = make_envelope(store, {}, io::current_envelope_version() + 1);
        auto report = io::inspect_restore_blob(blob);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::unsupported_envelope_version);
        REQUIRE(report.envelope_version == io::current_envelope_version() + 1);
    }

    SECTION("payload size mismatch") {
        auto store = legacy_store_blob();
        auto blob = make_envelope(store);
        blob.push_back(0x7Fu);
        auto report = io::inspect_restore_blob(blob);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::payload_size_mismatch);
    }

    SECTION("declared payload sizes overflow the available envelope") {
        std::vector<std::uint8_t> blob;
        blob.insert(blob.end(), kEnvelopeMagic, kEnvelopeMagic + 4);
        append_u32(blob, io::current_envelope_version());
        append_u32(blob, 0xFFFFFFFFu);
        append_u32(blob, 0xFFFFFFFFu);
        append_u32(blob, 0);
        auto report = io::inspect_restore_blob(blob);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::payload_size_mismatch);
    }

    SECTION("crc mismatch") {
        auto store = legacy_store_blob();
        auto blob = make_envelope(store);
        blob.back() ^= 0x44u;
        auto report = io::inspect_restore_blob(blob);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::crc_mismatch);
    }

    SECTION("empty store payload") {
        const std::array<std::uint8_t, 4> plugin = {'K', 'E', 'E', 'P'};
        auto blob = make_envelope({}, plugin);
        auto report = io::inspect_restore_blob(blob);
        REQUIRE_FALSE(report.ok());
        REQUIRE(report.status == io::RestoreDiagnosticStatus::empty_store_payload);
        REQUIRE(report.requires_plugin_restore);
        REQUIRE(report.may_require_resource_relink);
    }
}

TEST_CASE("plugin_state_io restore diagnostics flag old envelopes without migration as unavailable",
          "[format][plugin-state][restore-diagnostics]") {
    namespace io = pulp::format::plugin_state_io;

    auto store = legacy_store_blob();
    auto blob = make_envelope(store, {}, 0);
    auto report = io::inspect_restore_blob(blob);

    REQUIRE_FALSE(report.ok());
    REQUIRE(report.status == io::RestoreDiagnosticStatus::envelope_migration_unavailable);
    REQUIRE(report.format == io::RestoreBlobFormat::envelope);
    REQUIRE(report.envelope_version == 0);
    REQUIRE(report.requires_envelope_migration);
    REQUIRE_FALSE(report.requires_plugin_restore);
}
