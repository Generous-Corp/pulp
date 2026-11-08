#pragma once

#include <string>
#include <vector>

// `pulp audio spectrogram <file> --png <out> [options]` — display-only STFT
// image with a `pulp.audio.image.v1` sidecar.
int cmd_audio_spectrogram(const std::vector<std::string>& args);
