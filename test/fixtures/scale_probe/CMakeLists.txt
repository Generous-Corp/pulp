# An instrument at product scale (2,000 parameters, six stereo outputs and a
# stereo input) built as a real VST3, CLAP and, on macOS, AU bundle. Hosting
# tests load it to prove the format adapters at that scale; see scale_probe.hpp.
if(PULP_IOS OR ANDROID OR EMSCRIPTEN OR WIN32)
    return()
endif()
if(NOT PULP_HAS_CLAP AND NOT PULP_HAS_VST3)
    return()
endif()

set(_scale_probe_formats "")
if(PULP_HAS_VST3)
    list(APPEND _scale_probe_formats VST3)
endif()
if(PULP_HAS_CLAP)
    list(APPEND _scale_probe_formats CLAP)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND _scale_probe_formats AU)
endif()

pulp_add_plugin(PulpScaleProbe
    NO_EDITOR
    FORMATS           ${_scale_probe_formats}
    PLUGIN_NAME       "Pulp Scale Probe"
    BUNDLE_ID         "dev.pulp.test.scale-probe"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PScp"
    MANUFACTURER_CODE "PlHp"
    SOURCES           scale_probe.hpp
    PROCESSOR_FACTORY pulp::test_fixtures::create_scale_probe
)
