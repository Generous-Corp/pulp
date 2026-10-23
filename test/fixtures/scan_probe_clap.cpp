// SPDX-License-Identifier: MIT
//
// A hand-written CLAP bundle for the scanner and graph-load tests, built as
// two modules from this one source:
//
//   probe  — a static initializer writes the file named by the
//            PULP_TEST_DLOPEN_MARKER environment variable the moment the
//            library is loaded, so a test can prove whether a path was opened
//            at all. The entry point prints a line to stdout, exposes one
//            descriptor and no instantiable plugin.
//   abort  — with PULP_SCAN_FIXTURE_ABORT_ON_INIT defined, clap_entry.init
//            aborts the process, the way a bundle with broken static state
//            takes down whatever scans it.

#include <clap/clap.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

struct MarkerOnLoad {
    MarkerOnLoad() {
        if (const char* marker = std::getenv("PULP_TEST_DLOPEN_MARKER")) {
            std::ofstream out(marker, std::ios::binary | std::ios::trunc);
            out << "loaded\n";
        }
    }
};
MarkerOnLoad marker_on_load;

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                 nullptr};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "dev.pulp.test.scan-probe",
    "Pulp Scan Probe",
    "Pulp",
    "",
    "",
    "",
    "1.0.0",
    "Scanner fixture with one descriptor",
    kFeatures,
};

uint32_t get_plugin_count(const clap_plugin_factory_t*) {
    return 1;
}

const clap_plugin_descriptor_t* get_plugin_descriptor(const clap_plugin_factory_t*,
                                                      uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t* create_plugin(const clap_plugin_factory_t*, const clap_host_t*, const char*) {
    return nullptr;
}

const clap_plugin_factory_t kFactory = {get_plugin_count, get_plugin_descriptor, create_plugin};

bool entry_init(const char*) {
    // Real plugins print to stdout from their entry points; the scan
    // protocol has to survive that.
    std::printf("scan-probe fixture: chatter on stdout during clap_entry.init\n");
    std::fflush(stdout);
#if defined(PULP_SCAN_FIXTURE_ABORT_ON_INIT)
    std::abort();
#else
    return true;
#endif
}

void entry_deinit() {}

const void* entry_get_factory(const char* factory_id) {
    return std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory,
};
