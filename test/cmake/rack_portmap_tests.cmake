# The Rack port-map harness (tools/rack/).
#
# Registered here rather than beside the Forge Modular example because the
# harness is pure Python with no Rack SDK in it: gating it on PULP_HAS_RACK
# would mean it never ran anywhere except a developer's Rack-enabled build,
# which for a check that guards a silent zero is the same as not having it.
if(Python3_Interpreter_FOUND)
    # Shape checks on the range-measuring harness. The two mistakes it covers
    # -- a scanner placed ahead of its subjects, and the port map's `modules`
    # read as a dict instead of a list -- both report "measured nothing" with
    # no error, and the full run that would catch them needs Rack, a Mac and
    # an audio device.
    add_test(NAME rack-measure-ranges
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_measure_ranges.py)
    set_tests_properties(rack-measure-ranges PROPERTIES
        LABELS "rack;portmap"
        TIMEOUT 60)
endif()
