// Sprite / skin asset resolution for the import CLI.
//
// After a design is parsed to DesignIR and the JS codegen options are fixed,
// this pass resolves every node's `asset_ref` against the envelope's asset
// manifest and derives the data-driven skin attributes codegen consumes:
// sprite-knob body hoisting, true-PNG-dimension stamping, opaque-core
// recovery for bleed sprites, sampled fader/meter skins, asset-bleed
// detection, and bundled-font path resolution. Everything is derived from the
// exported pixels themselves — the pass hardcodes nothing.

#pragma once

#include <pulp/view/design_ir.hpp>

#include <string>

namespace pulp::import_design {

/// Resolve sprite/knob/fader/meter skins and asset paths in-place on `ir`.
///
/// `input_file` anchors relative `local_path` manifest entries (the envelope's
/// parent directory); empty input or an empty asset manifest reduces the pass
/// to the knob-hoist step alone. `use_silver_knobs` keeps the native vector
/// knob (the sprite hoist only runs when it is false); `skin_faders` /
/// `skin_meters` gate the PNG-sampled skin derivation per widget kind.
void resolve_sprite_skins(pulp::view::DesignIR& ir,
                          const std::string& input_file,
                          bool use_silver_knobs,
                          bool skin_faders,
                          bool skin_meters);

}  // namespace pulp::import_design
