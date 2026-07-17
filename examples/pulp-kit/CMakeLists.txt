# PulpKit — a circuit-modelled analog drum machine.
#
# One polyphonic Processor owning one instance of every kit voice, each a struck
# or gated circuit model (never a sample). Voices are header-only under voices/;
# the tonal voices (kick, toms, snare bodies, rim/clave) reuse the bridged-T
# resonator from core/signal, and the membrane voice reuses VaDrum's own voice
# header — so this target needs the sibling va-drum example on its include path.
#
# The AU entry declares its class as PulpKitAU, which the component factory
# derives from this target's name — renaming the target without renaming the
# class breaks the factory symbol at load time, not at build time.

set(PULP_KIT_FORMATS CLAP)
if(PULP_HAS_VST3)
    list(APPEND PULP_KIT_FORMATS VST3)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND PULP_KIT_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()

pulp_add_plugin(PulpKit
    FORMATS           ${PULP_KIT_FORMATS}
    PLUGIN_NAME       "PulpKit"
    BUNDLE_ID         "com.pulp.pulp-kit"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PKit"
    MANUFACTURER_CODE "Pulp"
)

# Every voice is built from pulp::signal primitives (bridged-T core, square-osc
# bank, biquad/svf), so the shared Core target carries that dependency. The
# membrane voice includes VaDrum's voice header, so the sibling example dir is
# on the interface include path too.
target_link_libraries(PulpKit_Core INTERFACE pulp::signal)
target_include_directories(PulpKit_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../va-drum)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpKit
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpKit.clap/Contents/MacOS/PulpKit'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpKit PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
