#include <catch2/catch_test_macros.hpp>

#include <pulp/playback/production_class.hpp>

using namespace pulp;

namespace {

playback::ProviderSelectorProgram selector(playback::ProviderKind kind) {
    playback::ProviderSelectorProgram provider;
    provider.selected = kind;
    provider.available_mask =
        static_cast<std::uint8_t>(1u << static_cast<unsigned>(kind));
    return provider;
}

} // namespace

TEST_CASE("every compiled content path today is produced in band", "[playback][production]") {
    for (const auto kind : {playback::ProviderKind::Arrangement, playback::ProviderKind::Launcher,
                            playback::ProviderKind::ExternalInput}) {
        const auto declaration = playback::provider_production_declaration(selector(kind));
        REQUIRE(declaration.mode == timeline::ProductionMode::Synchronous);
        REQUIRE(declaration.lookahead_ms == 0);
        REQUIRE(timeline::validate_production_declaration(declaration) ==
                timeline::ProductionDeclarationErrorCode::None);
    }
}

TEST_CASE("document-carried providers claim bit-reproducible replay, live input does not",
          "[playback][production]") {
    REQUIRE(playback::provider_production_declaration(selector(playback::ProviderKind::Arrangement))
                .reproducibility == timeline::ReproducibilityClass::Deterministic);
    REQUIRE(playback::provider_production_declaration(selector(playback::ProviderKind::Launcher))
                .reproducibility == timeline::ReproducibilityClass::Deterministic);
    // Live input is not carried by the document, so no second render of the
    // document can claim to reproduce it.
    const auto external =
        playback::provider_production_declaration(selector(playback::ProviderKind::ExternalInput));
    REQUIRE(external.reproducibility == timeline::ReproducibilityClass::BestEffort);
    REQUIRE_FALSE(timeline::is_bit_reproducible(external.reproducibility));
}
