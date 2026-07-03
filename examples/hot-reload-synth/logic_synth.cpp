// Hot-reloadable DSP "logic" for the hot-reload SYNTH demo (instrument variant).
//
// This is the half you EDIT + recompile while the host keeps playing notes. The
// shell (a ReloadableShell built into the loaded AU/VST3/CLAP instrument) watches
// the compiled .dylib and hot-swaps this synth in live — hold a chord and hear
// the timbre morph with no reopen.
//
// The synth itself lives in synth_voices.hpp (so it's unit-testable). The one
// thing you EDIT LIVE is the OSCILLATOR SHAPE below: Sine is pure/soft, Saw is
// bright/buzzy — a night-and-day timbre change (unlike the subtler tremolo demo).
// Flip kOsc, run rebuild_logic.sh, and the held notes change character under your
// ears, click-free (the crossfade + RT-safe swap engine).

#include "synth_voices.hpp"
#include <pulp/format/reload/reload_abi.hpp>

namespace {
// ── EDIT ME LIVE ────────────────────────────────────────────────────────────
constexpr pulp::examples::Osc kOsc = pulp::examples::Osc::Sine;
// ─────────────────────────────────────────────────────────────────────────────
}  // namespace

PULP_RELOAD_LOGIC(new pulp::examples::PolySynth(kOsc))
