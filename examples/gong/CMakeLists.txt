# Gong — a struck thin-plate modal cloud with a bounded nonlinear bloom.
# Validates: a dense inharmonic modal bank (hundreds of modes) plus a block-rate,
# energy-conserving three-wave coupling that reproduces the amplitude-dependent
# spectral bloom of a tam-tam — the one nonlinearity no FFT pipeline can express.
#
# The AU entry declares its class as GongAU, which the component factory derives
# from this target's name — renaming the target without renaming the class breaks
# the factory symbol at load time, not at build time.

set(GONG_FORMATS CLAP)
if(PULP_HAS_VST3)
    list(APPEND GONG_FORMATS VST3)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND GONG_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()

pulp_add_plugin(Gong
    FORMATS           ${GONG_FORMATS}
    PLUGIN_NAME       "Gong"
    BUNDLE_ID         "com.pulp.gong"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PGng"
    MANUFACTURER_CODE "Pulp"
)

# The plate is built from pulp::signal primitives (ModalBankT's coupled-form
# core lives in core/signal), so the shared Core target carries the dependency.
target_link_libraries(Gong_Core INTERFACE pulp::signal)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-Gong
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/Gong.clap/Contents/MacOS/Gong'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-Gong PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
