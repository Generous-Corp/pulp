#include <catch2/catch_test_macros.hpp>
#include <pulp/host/voice_template.hpp>

#include <cmath>
#include <limits>
#include <string>

using namespace pulp::host;

namespace {
VoiceTemplateSchema schema() {
    VoiceTemplateSchema s;
    s.template_namespace = "com.pulp";
    s.template_name = "voice";
    s.template_revision = 2;
    s.bank_schema_version = 1;
    s.bindings = {
        {{"filter", "cutoff"},
         "value",
         VoiceBindingTarget::Parameter,
         VoiceBindingType::Control,
         VoiceBindingDirection::Input,
         VoiceBindingShape::Scalar,
         VoiceBindingRate::Block,
         false,
         0},
        {{"audio", "out"},
         "main",
         VoiceBindingTarget::Port,
         VoiceBindingType::Audio,
         VoiceBindingDirection::Output,
         VoiceBindingShape::Stereo,
         VoiceBindingRate::Sample,
         false,
         0},
    };
    return s;
}

VoiceBankState state_for(const VoiceBankManifest& manifest, float value) {
    VoiceBankState state;
    state.manifest_sha256 = manifest.sha256;
    state.parameters.push_back({3, {"filter", "cutoff"}, "value", value});
    return state;
}

VoiceTemplateSchema wide_schema(std::size_t count) {
    VoiceTemplateSchema s;
    s.template_namespace = "com.pulp";
    s.template_name = "wide-voice";
    s.template_revision = 2;
    s.bank_schema_version = 1;
    s.bindings.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        s.bindings.push_back({{"parameter", std::to_string(i)},
                              "value",
                              VoiceBindingTarget::Parameter,
                              VoiceBindingType::Control,
                              VoiceBindingDirection::Input,
                              VoiceBindingShape::Scalar,
                              VoiceBindingRate::Block,
                              false,
                              0});
    }
    return s;
}

VoiceBankState wide_state(const VoiceBankManifest& manifest, std::size_t declaration_count,
                          std::size_t count) {
    VoiceBankState state;
    state.manifest_sha256 = manifest.sha256;
    state.parameters.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        state.parameters.push_back({static_cast<std::uint32_t>(i / declaration_count),
                                    {"parameter", std::to_string(i % declaration_count)},
                                    "value",
                                    static_cast<float>(i)});
    return state;
}
} // namespace

TEST_CASE("G4 voice template expands eight isolated stable bindings") {
    const auto prepared = prepare_voice_bank(schema());
    REQUIRE(prepared.valid);
    REQUIRE(prepared.manifest.bindings.size() == 16);
    CHECK_FALSE(prepared.manifest.sha256.empty());
    CHECK(prepared.manifest.bindings.front().slot_ordinal == 0);
    CHECK(prepared.manifest.bindings.back().slot_ordinal == 7);
    CHECK(prepared.manifest.bindings.front().identity.valid);

    auto reordered = schema();
    std::swap(reordered.bindings[0], reordered.bindings[1]);
    const auto second = prepare_voice_bank(reordered);
    REQUIRE(second.valid);
    CHECK(second.manifest.sha256 == prepared.manifest.sha256);
}

TEST_CASE("G4 unused parameter ordinals canonicalize identity and manifest digest") {
    auto ordinal = schema();
    ordinal.bindings[0].ordinal = 91;
    const auto canonical = prepare_voice_bank(schema());
    const auto equivalent = prepare_voice_bank(ordinal);
    REQUIRE(canonical.valid);
    REQUIRE(equivalent.valid);
    CHECK(canonical.manifest.sha256 == equivalent.manifest.sha256);
    CHECK(canonical.manifest.bindings[1].spec.ordinal == 0);
    CHECK(equivalent.manifest.bindings[1].spec.ordinal == 0);
    CHECK(canonical.manifest.bindings[1].identity.encoding ==
          equivalent.manifest.bindings[1].identity.encoding);
}

TEST_CASE("G4 voice template persistence restores atomically and supports edit undo") {
    const auto prepared = prepare_voice_bank(schema());
    REQUIRE(prepared.valid);
    auto before = state_for(prepared.manifest, 0.25f);
    auto receipt = encode_voice_bank_state(prepared.manifest, before);
    REQUIRE(receipt.valid);
    CHECK(receipt.encoding.front() == 0x01);
    CHECK(receipt.sha256.size() == 64);
    VoiceBankState decoded;
    REQUIRE(decode_voice_bank_state(receipt.encoding, decoded));
    CHECK(decoded.manifest_sha256 == before.manifest_sha256);
    REQUIRE(decoded.parameters.size() == 1);
    CHECK(decoded.parameters.front().path == before.parameters.front().path);
    CHECK(decoded.parameters.front().target == before.parameters.front().target);
    const auto reencoded = encode_voice_bank_state(prepared.manifest, decoded);
    REQUIRE(reencoded.valid);
    CHECK(reencoded.encoding == receipt.encoding);
    CHECK(reencoded.sha256 == receipt.sha256);

    VoiceBankState live = state_for(prepared.manifest, 0.5f);
    REQUIRE(restore_voice_bank_state(prepared.manifest, before, live));
    CHECK(live.parameters.front().value == 0.25f);
    auto undo = live;
    live.parameters.front().value = 0.75f;
    REQUIRE(restore_voice_bank_state(prepared.manifest, undo, live));
    CHECK(live.parameters.front().value == 0.25f);
    auto truncated = receipt.encoding;
    truncated.pop_back();
    auto decode_destination = state_for(prepared.manifest, 0.9f);
    const auto decode_unchanged = decode_destination;
    CHECK_FALSE(decode_voice_bank_state(truncated, decode_destination));
    REQUIRE(decode_destination.parameters.size() == decode_unchanged.parameters.size());
    CHECK(decode_destination.parameters.front().value == decode_unchanged.parameters.front().value);
    auto trailing = receipt.encoding;
    trailing.push_back(0);
    CHECK_FALSE(decode_voice_bank_state(trailing, decoded));
    auto oversize = receipt.encoding;
    const auto count_offset =
        std::size_t{1} + 4 + std::string("pulp.graph.voice-bank.state").size() + 4 + 4 + 64;
    REQUIRE(oversize.size() >= count_offset + 4);
    oversize[count_offset] = 0;
    oversize[count_offset + 1] = 0;
    oversize[count_offset + 2] = 0x10;
    oversize[count_offset + 3] = 0x01;
    CHECK_FALSE(decode_voice_bank_state(oversize, decoded));
}

TEST_CASE("G4 persistence keeps the 4096 parameter bound transactional") {
    constexpr std::size_t kAtLimitDeclarations = 512;
    constexpr std::size_t kAtLimitRecords = 4096;
    const auto at_limit = prepare_voice_bank(wide_schema(kAtLimitDeclarations));
    REQUIRE(at_limit.valid);
    auto bounded = wide_state(at_limit.manifest, kAtLimitDeclarations, kAtLimitRecords);
    const auto bounded_receipt = encode_voice_bank_state(at_limit.manifest, bounded);
    REQUIRE(bounded_receipt.valid);
    VoiceBankState bounded_decoded;
    REQUIRE(decode_voice_bank_state(bounded_receipt.encoding, bounded_decoded));
    CHECK(bounded_decoded.parameters.size() == kAtLimitRecords);

    VoiceBankState bounded_live = bounded_decoded;
    REQUIRE(restore_voice_bank_state(at_limit.manifest, bounded_decoded, bounded_live));
    CHECK(bounded_live.parameters.size() == kAtLimitRecords);

    constexpr std::size_t kOverLimitDeclarations = 513;
    constexpr std::size_t kOverLimitRecords = 4097;
    const auto over_limit = prepare_voice_bank(wide_schema(kOverLimitDeclarations));
    REQUIRE(over_limit.valid);
    auto oversized = wide_state(over_limit.manifest, kOverLimitDeclarations, kOverLimitRecords);
    CHECK_FALSE(encode_voice_bank_state(over_limit.manifest, oversized).valid);
    VoiceBankState destination = wide_state(over_limit.manifest, kOverLimitDeclarations, 1);
    const auto unchanged = destination;
    CHECK_FALSE(restore_voice_bank_state(over_limit.manifest, oversized, destination));
    REQUIRE(destination.parameters.size() == unchanged.parameters.size());
    CHECK(destination.parameters.front().value == unchanged.parameters.front().value);
}

TEST_CASE(
    "G4 planted negatives refuse aliases, cross-slot mutation, mismatch, and failed migration") {
    auto duplicate = schema();
    duplicate.bindings.push_back(duplicate.bindings.front());
    auto rejected = prepare_voice_bank(duplicate);
    CHECK_FALSE(rejected.valid);

    auto ordinal_duplicate = schema();
    ordinal_duplicate.bindings.push_back(ordinal_duplicate.bindings.front());
    ordinal_duplicate.bindings.back().ordinal = 9;
    CHECK_FALSE(prepare_voice_bank(ordinal_duplicate).valid);

    auto alias = schema();
    alias.bindings[0].path[0] = "filte\xCC\x81r";
    alias.bindings.push_back(alias.bindings[0]);
    alias.bindings.back().path[0] = "filtér";
    CHECK_FALSE(prepare_voice_bank(alias).valid);

    auto shared = schema();
    shared.bindings[0].shared_across_slots = true;
    CHECK_FALSE(prepare_voice_bank(shared).valid);

    auto mismatch = schema();
    mismatch.bindings[1].type = VoiceBindingType::Event;
    mismatch.bindings[1].rate = VoiceBindingRate::Sample;
    CHECK_FALSE(prepare_voice_bank(mismatch).valid);
    mismatch = schema();
    mismatch.bindings[1].shape = VoiceBindingShape::Scalar;
    CHECK_FALSE(prepare_voice_bank(mismatch).valid);

    const auto prepared = prepare_voice_bank(schema());
    REQUIRE(prepared.valid);
    auto forged_manifest = prepared.manifest;
    forged_manifest.bindings.front().identity.sha256[0] =
        forged_manifest.bindings.front().identity.sha256[0] == '0' ? '1' : '0';
    auto forged_state = state_for(forged_manifest, 0.5f);
    CHECK_FALSE(encode_voice_bank_state(forged_manifest, forged_state).valid);
    forged_manifest = prepared.manifest;
    forged_manifest.bindings.front().identity.valid = false;
    CHECK_FALSE(encode_voice_bank_state(forged_manifest, forged_state).valid);
    forged_manifest = prepared.manifest;
    forged_manifest.bindings.front().slot_ordinal = 7;
    CHECK_FALSE(encode_voice_bank_state(forged_manifest, forged_state).valid);
    forged_manifest = prepared.manifest;
    forged_manifest.bindings.front().spec.shared_across_slots = true;
    CHECK_FALSE(encode_voice_bank_state(forged_manifest, forged_state).valid);
    auto live = state_for(prepared.manifest, 0.5f);
    const auto unchanged = live;
    auto failed = state_for(prepared.manifest, 0.8f);
    failed.manifest_sha256 = "stale";
    CHECK_FALSE(restore_voice_bank_state(prepared.manifest, failed, live));
    CHECK(live.parameters.front().value == unchanged.parameters.front().value);
    failed = state_for(prepared.manifest, 0.8f);
    failed.parameters.front().value = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(restore_voice_bank_state(prepared.manifest, failed, live));
    CHECK(live.parameters.front().value == unchanged.parameters.front().value);
    auto alias_schema = schema();
    alias_schema.bindings[0].path[0] = "filte\xCC\x81r";
    const auto alias_prepared = prepare_voice_bank(alias_schema);
    REQUIRE(alias_prepared.valid);
    VoiceBankState alias_state;
    alias_state.manifest_sha256 = alias_prepared.manifest.sha256;
    alias_state.parameters = {{3, {"filte\xCC\x81r"}, "value", 0.8f},
                              {3, {"filtér"}, "value", 0.9f}};
    VoiceBankState alias_live = alias_state;
    CHECK_FALSE(restore_voice_bank_state(alias_prepared.manifest, alias_state, alias_live));
}
