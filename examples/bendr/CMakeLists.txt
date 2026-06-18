# Bendr — example effect built entirely from public pulp::signal primitives.
#
# Demonstrates composing RealtimePitchTimeProcessor (with built-in FreezeHold +
# TransientPhasePolicy), PitchedFeedbackDelay, and DryWetMixer into a realtime
# pitch / time / freeze effect with a native GPU UI — all from core/signal,
# no third-party DSP.

pulp_add_plugin(Bendr
    FORMATS           VST3 AU CLAP Standalone
    SOURCES           src/reference_view.cpp
    PLUGIN_NAME       "Bendr"
    BUNDLE_ID         "com.pulp.examples.bendr"
    MANUFACTURER      "Pulp"
    VERSION           "0.1.0"
    CATEGORY          Effect
    PLUGIN_CODE       "Bnd2"
    MANUFACTURER_CODE "Pulp"
    ACCEPTS_MIDI
)

# The processor's create_view() pulls in the view + canvas stack, and the
# source includes its headers from src/.
if(TARGET Bendr_Core)
    target_include_directories(Bendr_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(Bendr_Core PUBLIC pulp::view pulp::canvas)
endif()

# CLAP load smoke test — the plugin's shared object must dlopen cleanly.
if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-Bendr
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Bendr.clap/Contents/MacOS/Bendr'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-Bendr PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
