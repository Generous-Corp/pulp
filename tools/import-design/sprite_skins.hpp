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

/// Make the generated artifact self-contained: copy every filesystem asset
/// the IR references (image/sprite `asset_path` attributes and bundled-font
/// `resolved_path`) into `<output dir>/assets/` and rewrite the references to
/// output-relative `assets/<file>` paths, in-place on `ir`.
///
/// The absolute paths stamped by resolve_sprite_skins point into the import's
/// decode scratch directory, which is deleted when the CLI exits — an export
/// that kept them would silently lose its images on any later render.
/// Renderers resolve the relative form against the script's own directory
/// (WidgetBridge::set_script_base_dir). Assets that cannot be copied keep
/// their absolute path (with a warning) rather than failing the import.
void localize_ir_assets(pulp::view::DesignIR& ir, const std::string& output_file);

}  // namespace pulp::import_design
