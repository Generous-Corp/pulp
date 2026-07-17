# ModalInstrument — one polyphonic, spec-driven modal instrument.
# Validates the platform thesis: an instrument is a ModalSpec (modes +
# excitation + position maps), and this one Processor plays any of them. It
# ships with a tuned-marimba spec built in so it makes sound out of the box.
#
# The AU entry declares its class as ModalInstrumentAU, which the component
# factory derives from this target's name — renaming the target without
# renaming the class breaks the factory symbol at load time, not at build time.

set(MODAL_INSTRUMENT_FORMATS CLAP)
if(PULP_HAS_VST3)
    list(APPEND MODAL_INSTRUMENT_FORMATS VST3)
endif()
if(APPLE AND PULP_HAS_AUSDK)
    list(APPEND MODAL_INSTRUMENT_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()

pulp_add_plugin(ModalInstrument
    FORMATS           ${MODAL_INSTRUMENT_FORMATS}
    PLUGIN_NAME       "ModalInstrument"
    BUNDLE_ID         "com.pulp.modal-instrument"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PMod"
    MANUFACTURER_CODE "Pulp"
)

# The voice is a ModalBankT struck by a contact pulse, resolved from a
# ModalSpec — both live in core/signal — so the shared Core target carries the
# dependency.
target_link_libraries(ModalInstrument_Core INTERFACE pulp::signal)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-ModalInstrument
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/ModalInstrument.clap/Contents/MacOS/ModalInstrument'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-ModalInstrument PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
