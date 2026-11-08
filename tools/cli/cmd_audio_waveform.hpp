#pragma once

#include <string>
#include <vector>

// `pulp audio waveform <file> --png <out> [options]` — display-only peak
// waveform image from WaveformOverview with a `pulp.audio.image.v1` sidecar.
int cmd_audio_waveform(const std::vector<std::string>& args);
