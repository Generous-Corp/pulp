#pragma once

#include <string>
#include <vector>

/// `pulp audio inspect <file>`: byte-level layout report for an audio file.
int cmd_audio_inspect(const std::vector<std::string>& args);
