#include <pulp/playback/production_class.hpp>

namespace pulp::playback {

timeline::ProductionDeclaration
provider_production_declaration(ProviderSelectorProgram provider) noexcept {
    timeline::ProductionDeclaration declaration;
    declaration.mode = timeline::ProductionMode::Synchronous;
    declaration.lookahead_ms = 0;
    // Weakest claim first, so a provider kind added without a case here reports
    // that it cannot be replayed rather than inheriting the strongest claim.
    declaration.reproducibility = timeline::ReproducibilityClass::BestEffort;
    switch (provider.selected) {
    case ProviderKind::Arrangement:
    case ProviderKind::Launcher:
        // Arranged and launched content is lowered from the document, so a
        // second render of the same revision reproduces it bit for bit.
        declaration.reproducibility = timeline::ReproducibilityClass::Deterministic;
        break;
    case ProviderKind::ExternalInput:
        // Live input is not carried by the document and cannot be replayed from
        // it, whatever the renderer currently emits for this provider.
        declaration.reproducibility = timeline::ReproducibilityClass::BestEffort;
        break;
    }
    return declaration;
}

timeline::ProductionDeclaration track_production_declaration(const TrackProgram& track) noexcept {
    return provider_production_declaration(track.provider());
}

timeline::ReproducibilityClass program_reproducibility(const PlaybackProgram& program) noexcept {
    auto claim = timeline::ReproducibilityClass::Deterministic;
    for (const auto& track : program.tracks()) {
        if (!track)
            continue;
        claim = timeline::weakest(claim, track_production_declaration(*track).reproducibility);
    }
    return claim;
}

} // namespace pulp::playback
