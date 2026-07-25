#pragma once

/// @file production_class.hpp
/// Derives what a compiled program may honestly claim about being replayed.
///
/// The claim is derived from the program rather than stored on it, so it cannot
/// drift from what the compiler actually lowered. Every content path the
/// compiler lowers today is produced in band, so every track reports
/// `Synchronous`; the reproducibility claim comes from the track's selected
/// provider, which is the one compiled field that already decides where a
/// track's audio comes from.

#include <pulp/playback/program.hpp>
#include <pulp/timeline/production_mode.hpp>

namespace pulp::playback {

/// What content served by @p provider declares about its production.
timeline::ProductionDeclaration
provider_production_declaration(ProviderSelectorProgram provider) noexcept;

/// What a single track's compiled content declares about its production.
timeline::ProductionDeclaration track_production_declaration(const TrackProgram& track) noexcept;

/// The weakest claim any track in the program is entitled to make. An empty
/// program renders silence, which is bit-reproducible.
timeline::ReproducibilityClass program_reproducibility(const PlaybackProgram& program) noexcept;

} // namespace pulp::playback
