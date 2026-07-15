#pragma once

#include <string>
#include <vector>

// `pulp audio plugin-inspect` — isolated parameter/API discovery for one
// hosted third-party plugin.
int cmd_audio_plugin_inspect(const std::vector<std::string>& args);
