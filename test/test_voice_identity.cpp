#include <catch2/catch_test_macros.hpp>
#include <pulp/host/voice_identity.hpp>

using namespace pulp::host;

namespace {
VoiceIdentityKey key() {
    VoiceIdentityKey result;
    result.template_namespace = "com.pulp";
    result.template_name = "bass";
    result.template_revision = 3;
    result.slot_ordinal = 2;
    result.path = {"node", "osc", "port"};
    result.has_port = true;
    result.port = {VoicePortDirection::Output, VoicePortKind::Audio, 0};
    return result;
}
} // namespace

TEST_CASE("G4 voice identity is stable across equivalent authored values") {
    auto first = key();
    auto second = key();
    first.template_name = "ba\xCC\x81ss";
    second.template_name = "b\xC3\xA1ss"; // NFC-equivalent composed spelling.
    const auto a = canonical_voice_identity(first);
    const auto b = canonical_voice_identity(second);
    REQUIRE(a.valid);
    REQUIRE(b.valid);
    CHECK(a.encoding == b.encoding);
    CHECK(a.sha256 == b.sha256);
    CHECK(a.encoding.front() == 0x01);
}

TEST_CASE("G4 voice identity changes for authored slot and target") {
    const auto baseline = canonical_voice_identity(key());
    REQUIRE(baseline.valid);
    auto changed = key();
    changed.slot_ordinal = 3;
    REQUIRE(canonical_voice_identity(changed).valid);
    CHECK(canonical_voice_identity(changed).sha256 != baseline.sha256);
    changed = key();
    changed.port.ordinal = 1;
    CHECK(canonical_voice_identity(changed).sha256 != baseline.sha256);
}

TEST_CASE("G4 planted negatives reject noncanonical or unbound identity") {
    auto missing = key();
    missing.has_port = false;
    missing.path.clear();
    CHECK_FALSE(canonical_voice_identity(missing).valid);
    auto overflow = key();
    overflow.slot_ordinal = 8;
    CHECK_FALSE(canonical_voice_identity(overflow).valid);
    auto bad_port = key();
    bad_port.port.kind = static_cast<VoicePortKind>(9);
    CHECK_FALSE(canonical_voice_identity(bad_port).valid);
    auto slash = key();
    slash.path[1] = "osc/port";
    CHECK_FALSE(canonical_voice_identity(slash).valid);
    auto empty_segment = key();
    empty_segment.path[1].clear();
    CHECK_FALSE(canonical_voice_identity(empty_segment).valid);
    auto whitespace_segment = key();
    whitespace_segment.path[1] = " \xE2\x80\x83";
    CHECK_FALSE(canonical_voice_identity(whitespace_segment).valid);
    auto malformed_path = key();
    malformed_path.path[1] = std::string("\xFF", 1);
    CHECK_FALSE(canonical_voice_identity(malformed_path).valid);
    auto malformed_parameter = key();
    malformed_parameter.has_port = false;
    malformed_parameter.has_parameter = true;
    malformed_parameter.parameter = std::string("\xC3", 1);
    CHECK_FALSE(canonical_voice_identity(malformed_parameter).valid);
    auto zero_revision = key();
    zero_revision.template_revision = 0;
    CHECK_FALSE(canonical_voice_identity(zero_revision).valid);
    auto zero_schema = key();
    zero_schema.bank_schema_version = 0;
    CHECK_FALSE(canonical_voice_identity(zero_schema).valid);
    auto invalid = key();
    invalid.template_namespace = std::string("bad\0name", 8);
    CHECK_FALSE(canonical_voice_identity(invalid).valid);
    auto alias = key();
    alias.path[1] = "\x65\xCC\x81";
    const auto alias_result = canonical_voice_identity(alias);
    REQUIRE(alias_result.valid);
    alias.path[1] = "é";
    CHECK(alias_result.sha256 == canonical_voice_identity(alias).sha256);
}
