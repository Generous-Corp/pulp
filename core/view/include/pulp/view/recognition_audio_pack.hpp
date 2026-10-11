#pragma once

/// @file recognition_audio_pack.hpp
/// The built-in Pulp Figma Library of audio controls (knob, fader, meter,
/// XY pad, waveform, spectrum) as a recognition pack for the generic
/// RecognitionResolver merge layer.
///
/// The resolver itself knows no control vocabulary; this pack is Pulp-owned
/// product data and is linked separately from the importer
/// (`pulp-design-import-audio-pack`). Callers add it as source 0 so the built-in
/// library is the lowest-precedence source and a user manifest or an installed
/// package's `design_controls` fragment can override any key.

#include <pulp/view/recognition_resolver.hpp>

namespace pulp::view {

/// The built-in library as one recognition source named "builtin-library".
/// Mirrors tools/figma-plugin/library-manifest.json; a unit test pins the two
/// against each other so they cannot drift.
RecognitionSource pulp_audio_control_library_source();

/// A resolver seeded with the built-in library as source 0. Equivalent to
/// `RecognitionResolver{}.add_source(pulp_audio_control_library_source())`.
RecognitionResolver recognition_resolver_with_builtin_library();

} // namespace pulp::view
