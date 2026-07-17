# VaDrum — a circuit-faithful TR-808 bass drum
# Validates: a persistent struck resonator driven by additive excitation, with
# no per-note voice allocation anywhere in the signal path.
#
# The AU entry declares its class as VaDrumAU, which the component factory
# derives from this target's name — renaming the target without renaming the
# class breaks the factory symbol at load time, not at build time.

set(VA_DRUM_FORMATS CLAP)
if(PULP_HAS_VST3)
    list(APPEND VA_DRUM_FORMATS VST3)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND VA_DRUM_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()

pulp_add_plugin(VaDrum
    FORMATS           ${VA_DRUM_FORMATS}
    PLUGIN_NAME       "VaDrum"
    BUNDLE_ID         "com.pulp.va-drum"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PVaD"
    MANUFACTURER_CODE "Pulp"
)

# The voice is built from pulp::signal primitives (the bridged-T core lives in
# core/signal), so the shared Core target carries the dependency.
target_link_libraries(VaDrum_Core INTERFACE pulp::signal)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-VaDrum
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/VaDrum.clap/Contents/MacOS/VaDrum'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-VaDrum PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
