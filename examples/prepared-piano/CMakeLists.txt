# PreparedPiano — a modal string with a movable one-sided preparation
# Validates: a struck modal string plus a single lumped point contact resolved
# in closed form each sample (no Newton iteration), exposing preparation TYPE,
# POSITION and STRENGTH as continuous per-note controls.
#
# The AU entry declares its class as PreparedPianoAU, which the component
# factory derives from this target's name — renaming the target without renaming
# the class breaks the factory symbol at load time, not at build time.

set(PREPARED_PIANO_FORMATS CLAP)
if(PULP_HAS_VST3)
    list(APPEND PREPARED_PIANO_FORMATS VST3)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND PREPARED_PIANO_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()

pulp_add_plugin(PreparedPiano
    FORMATS           ${PREPARED_PIANO_FORMATS}
    PLUGIN_NAME       "PreparedPiano"
    BUNDLE_ID         "com.pulp.prepared-piano"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PPrP"
    MANUFACTURER_CODE "Pulp"
)

# The voice is built from pulp::signal primitives (the modal bank + denormal
# snap live in core/signal), so the shared Core target carries the dependency.
target_link_libraries(PreparedPiano_Core INTERFACE pulp::signal)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PreparedPiano
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PreparedPiano.clap/Contents/MacOS/PreparedPiano'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PreparedPiano PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
