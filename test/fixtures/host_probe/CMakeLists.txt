# A real VST3, CLAP, and (on macOS) AU bundle that reports what its host gave it (transport,
# process mode, sample rate, block size, sidechain, mapped MIDI) through
# read-only parameters, and on command changes its own parameter or its
# latency. Hosting tests load it through PluginSlot; see host_probe.hpp.
if(PULP_IOS OR ANDROID OR EMSCRIPTEN OR WIN32)
    return()
endif()
if(NOT PULP_HAS_CLAP AND NOT PULP_HAS_VST3)
    return()
endif()

set(_host_probe_formats "")
if(PULP_HAS_VST3)
    list(APPEND _host_probe_formats VST3)
endif()
if(PULP_HAS_CLAP)
    list(APPEND _host_probe_formats CLAP)
endif()
# The AU component is what the AUAudioUnit slot hosts: registered in process by
# the test, or installed for the out-of-process run.
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND _host_probe_formats AU)
endif()

pulp_add_plugin(PulpHostProbe
    NO_EDITOR
    FORMATS           ${_host_probe_formats}
    PLUGIN_NAME       "Pulp Host Probe"
    BUNDLE_ID         "dev.pulp.test.host-probe"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PHpr"
    MANUFACTURER_CODE "PlHp"
    SOURCES           host_probe.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_host_probe
)
