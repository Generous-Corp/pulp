#pragma once

#include <string>
#include <vector>

/// `pulp audio formats [--json]`: lists the readers and writers this build
/// registers, with what each one reads or writes.
int cmd_audio_formats(const std::vector<std::string>& args);
