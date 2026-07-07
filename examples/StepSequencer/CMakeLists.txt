# StepSequencer — synth-class step sequencer proving the
# StepGridView <-> SequencerStateChannel <-> Processor round-trip.
# The authoritative end-to-end proof is test/test_step_sequencer_roundtrip.cpp;
# this target packages the same Processor as a loadable CLAP instrument.

pulp_add_plugin(StepSequencer
    FORMATS         CLAP
    PLUGIN_NAME     "StepSequencer"
    BUNDLE_ID       "com.pulp.stepsequencer"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Instrument
    PLUGIN_CODE     "PStq"
    MANUFACTURER_CODE "Pulp"
)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-StepSequencer
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/StepSequencer.clap/Contents/MacOS/StepSequencer'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-StepSequencer PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
