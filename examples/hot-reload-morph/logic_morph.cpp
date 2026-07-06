// Hot-reload MORPH demo — the DSP+UI "logic" you swap live.
//
// Build it two ways (the build flips -DMORPH_HARSH); each is a *different plugin*
// behind the SAME parameter contract (Depth id1, Rate id2) so the shell accepts
// the hot-swap:
//
//   Version A "WARM"   — a gentle SINE tremolo + a calm blue editor.
//   Version B "HARSH"  — a hard SQUARE chop  + an aggressive red editor.
//
// Reloading swaps BOTH the sound and the look from one swap. The synth/DSP + the
// editor live in morph_dsp.hpp (so both variants are unit-testable in one binary);
// here we just pick the variant the build compiles.

#include "morph_dsp.hpp"
#include <pulp/format/reload/reload_abi.hpp>

#if defined(MORPH_HARSH)
PULP_RELOAD_LOGIC(new pulp::examples::MorphDsp(pulp::examples::kMorphHarsh))
#else
PULP_RELOAD_LOGIC(new pulp::examples::MorphDsp(pulp::examples::kMorphWarm))
#endif
