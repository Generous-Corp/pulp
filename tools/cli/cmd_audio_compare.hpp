// cmd_audio_compare.hpp — `pulp audio compare` dispatch.
//
// The agent-facing, ADVISORY before/after audio judgment. It delegates to the
// opt-in Audio Quality Lab tool (tools/audio/quality-lab, a managed Python
// venv) rather than reimplementing DSP in the CLI — so the FFT/analysis stays
// tool-side and the MIT core/CLI never depends on numpy/soundfile. Level-matches
// the candidate to the reference, runs one curated axis (--profile), and prints
// a typed evidence envelope + an action-oriented verdict.
//
// Distinct from `pulp audio validate compare`: THAT is the deterministic gate
// primitive (exact/null/spectral WAV diff, pass/fail). THIS is the interpreted,
// advisory judgment an agent acts on — it exits non-zero only when it could not
// measure (invalid input), never for a judgment.

#pragma once

#include <string>
#include <vector>

// Dispatch `pulp audio compare <reference.wav> <candidate.wav> [options]`.
// `args` is the tail AFTER the "compare" token. Returns the tool's exit code
// (2 == could-not-measure/invalid; 0 == measured a judgment), or a setup error
// code when the opt-in tool is not installed.
int cmd_audio_compare(const std::vector<std::string>& args);
