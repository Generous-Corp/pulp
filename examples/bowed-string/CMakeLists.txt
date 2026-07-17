# BowedString — a McIntyre-Schumacher-Woodhouse digital-waveguide bowed string
# Validates: a self-sustaining stick-slip friction junction between two
# fractional-delay string segments — a nonlinear feedback loop that holds a
# bounded Helmholtz limit cycle (sustains, does not decay) and cannot run away.
#
# The AU entry declares its class as BowedStringAU, which the component factory
# derives from this target's name — renaming the target without renaming the
# class breaks the factory symbol at load time, not at build time.

set(BOWED_STRING_FORMATS CLAP)
if(PULP_HAS_VST3)
    list(APPEND BOWED_STRING_FORMATS VST3)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND BOWED_STRING_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()

pulp_add_plugin(BowedString
    FORMATS           ${BOWED_STRING_FORMATS}
    PLUGIN_NAME       "BowedString"
    BUNDLE_ID         "com.pulp.bowed-string"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PBoS"
    MANUFACTURER_CODE "Pulp"
)

# The voice reads the delays with the stateless Lagrange helper and snaps
# denormals — both live in core/signal — so the shared Core target carries the
# dependency.
target_link_libraries(BowedString_Core INTERFACE pulp::signal)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-BowedString
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/BowedString.clap/Contents/MacOS/BowedString'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-BowedString PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
