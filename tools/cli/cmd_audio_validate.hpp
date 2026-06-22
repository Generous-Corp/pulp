// cmd_audio_validate.hpp — `pulp audio validate <verb>` dispatch.
//
// The validation namespace nested under `pulp audio` analyzes captured WAVs
// and stored artifact bundles using the reusable pulp::audio-analysis lib —
// it does NOT instantiate a Processor because the generic CLI is not tied to
// a plugin. Verbs:
//   summarize <file.wav> [--json]          — agent-readable signal summary
//   doctor    <file.wav> [--thd] [--response f1,f2,...] [options]
//   compare   <a.wav> <b.wav> [--mode null|spectral] [--tolerance <dbfs>]
//   assert    <dir-or-assertions.json>     — re-check stored assertions
//
// Existing `pulp audio model/excerpt-find/read-bundle` behavior is untouched;
// this is reached only when args[0] == "validate".

#pragma once

#include <string>
#include <vector>

// Dispatch `pulp audio validate <verb> ...`. `args` is the tail AFTER the
// "validate" token (i.e. starts at the verb). Returns a process exit code.
int cmd_audio_validate(const std::vector<std::string>& args);
