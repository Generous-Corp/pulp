// Isolated-slot query for the sample-region web profile.
//
// The curated WAM/WebCLAP source closure (tools/cmake/PulpSampleRegionWebSources.cmake)
// compiles signal_graph_authoring.cpp and signal_graph_prepare.cpp, which ask
// is_isolated_plugin_slot() of every plugin node. The real definition lives in
// isolated_plugin_slot.cpp next to load_isolated_clap(), the only factory for
// isolated slots; that file needs worker processes, memory-mapped files and
// threads, so the web profile does not compile it. Without that factory no
// isolated slot can exist in a web module, so the answer is always false.
// Native builds compile isolated_plugin_slot.cpp and never this file.

#include <pulp/host/isolated_plugin_slot.hpp>

namespace pulp::host {

bool is_isolated_plugin_slot(const PluginSlot&) noexcept { return false; }

} // namespace pulp::host
