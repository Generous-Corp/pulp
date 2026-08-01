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

# What does packaging this sequencer as a plugin actually cost, and what does
# the binary certainly contain? TIER is the ceiling — nothing outside it or
# PULP_LINK_FLOOR_DEBT_StepSequencer_CLAP in tools/cmake/PulpLinkFloor.cmake may
# be reached. REQUIRE is the floor, and it is deliberately narrow here: this
# plugin's own code adds no module edge beyond the adapter it is packaged as, so
# `format` (it is a loadable plugin) and `state` (it has a parameter store) are
# what can be claimed. Notably absent is timeline_editor — this target does not
# link the piano roll, and a tier that named it would have asserted a link the
# binary never had. The report is written to ${CMAKE_BINARY_DIR}/link-floor/.
# The COMMAND guard keeps this file usable when the example is copied out of the
# tree: the check is a repo-internal tool and is not shipped in the installed SDK.
if(TARGET StepSequencer_CLAP AND COMMAND pulp_assert_link_floor)
    pulp_report_link_closure(StepSequencer_CLAP)
    pulp_assert_link_floor(StepSequencer_CLAP
        TIER    sequencer-plugin
        REQUIRE format state)
endif()

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-StepSequencer
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/StepSequencer.clap/Contents/MacOS/StepSequencer'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-StepSequencer PROPERTIES LABELS "validation;clap" TIMEOUT 10)
endif()
