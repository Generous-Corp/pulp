// SPDX-License-Identifier: MIT
// Hermetic CLAP factory; filename selects deliberate ABI/identity faults.
#include <clap/clap.h>
#include <cstring>
#include <string>

namespace {
std::string mode;
const char* features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t first{CLAP_VERSION_INIT,
                                     "org.pulp.identity.first",
                                     "Same name",
                                     "Pulp",
                                     "",
                                     "",
                                     "",
                                     "1.0",
                                     "Identity fixture",
                                     features};
const clap_plugin_descriptor_t second{CLAP_VERSION_INIT,
                                      "org.pulp.identity.second",
                                      "Same name",
                                      "Pulp",
                                      "",
                                      "",
                                      "",
                                      "1.0",
                                      "Identity fixture",
                                      features};
clap_plugin_descriptor_t faulty;
bool CLAP_ABI init_entry(const char* path) {
    mode = path;
    return true;
}
void CLAP_ABI deinit_entry() {}
uint32_t CLAP_ABI count(const clap_plugin_factory_t*) {
    return mode.find("capped") != std::string::npos ? 1025 : 2;
}
const clap_plugin_descriptor_t* CLAP_ABI descriptor(const clap_plugin_factory_t*, uint32_t index) {
    if (index == 0)
        return &first;
    if (mode.find("hole") != std::string::npos)
        return nullptr;
    if (mode.find("duplicate") != std::string::npos)
        return &first;
    faulty = second;
    if (mode.find("null") != std::string::npos)
        faulty.id = nullptr;
    if (mode.find("empty") != std::string::npos)
        faulty.id = "";
    if (mode.find("invalid") != std::string::npos)
        faulty.id = "org.pulp.\xc0\xaf";
    return &faulty;
}
bool CLAP_ABI init_plugin(const clap_plugin_t*) {
    return mode.find("initfail") == std::string::npos;
}
void CLAP_ABI destroy(const clap_plugin_t* plugin) {
    delete plugin;
}
const void* CLAP_ABI extension(const clap_plugin_t*, const char*) {
    return nullptr;
}
const clap_plugin_t* CLAP_ABI create(const clap_plugin_factory_t*, const clap_host_t*,
                                     const char* id) {
    if (mode.find("createfail") != std::string::npos)
        return nullptr;
    auto* plugin = new clap_plugin_t{};
    plugin->desc = std::strcmp(id, second.id) == 0 ? &second : &first;
    if (mode.find("mismatch") != std::string::npos)
        plugin->desc = &first;
    plugin->init = init_plugin;
    plugin->destroy = destroy;
    plugin->get_extension = extension;
    return plugin;
}
const clap_plugin_factory_t factory{count, descriptor, create};
const void* CLAP_ABI get_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
}
} // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, init_entry,
                                                            deinit_entry, get_factory};
