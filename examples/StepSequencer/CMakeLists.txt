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

# What does putting a piano roll in a plugin actually cost? The tier names the
# rungs a sequencer plugin claims; PULP_LINK_FLOOR_DEBT_StepSequencer_CLAP in
# tools/cmake/PulpLinkFloor.cmake records what the format packaging charges on
# top of them. The report is written to ${CMAKE_BINARY_DIR}/link-floor/.
# The COMMAND guard keeps this file usable when the example is copied out of the
# tree: the check is a repo-internal tool and is not shipped in the installed SDK.
if(TARGET StepSequencer_CLAP AND COMMAND pulp_assert_link_floor)
    pulp_report_link_closure(StepSequencer_CLAP)
    pulp_assert_link_floor(StepSequencer_CLAP TIER sequencer-plugin)
endif()

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-StepSequencer
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/StepSequencer.clap/Contents/MacOS/StepSequencer'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-StepSequencer PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
