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

    # Physical-value conversion for a vendor's controls. The coefficients it
    # asserts against were measured off real modules rather than invented, so
    # this catches a conversion that is present and wrong -- which is what a
    # book's "40 Hz" meets when it reaches a knob calibrated in volts. Its
    # live section round-trips whatever this machine has measured and skips
    # cleanly where there is no port map.
    add_test(NAME rack-param-units
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_param_units.py)
    set_tests_properties(rack-param-units PROPERTIES
        LABELS "rack;portmap"
        TIMEOUT 60)

    # The consumer proof: a model-facing physical target travels through
    # patch.py and becomes the raw knob value Rack will write. This uses the
    # measured Fundamental/VCO Hz-to-semitone mapping so copying the physical
    # number directly cannot pass.
    add_test(NAME rack-physical-targets
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_physical_targets.py)
    set_tests_properties(rack-physical-targets PROPERTIES
        LABELS "rack;portmap"
        TIMEOUT 60)

    foreach(_rack_safety_test IN ITEMS
            knowledge-admission
            recover-subset-font-pdf
            corpus-audit)
        string(REPLACE "-" "_" _rack_safety_file "${_rack_safety_test}")
        add_test(NAME rack-${_rack_safety_test}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_${_rack_safety_file}.py)
        set_tests_properties(rack-${_rack_safety_test} PROPERTIES
            LABELS "rack;safety"
            TIMEOUT 60)
    endforeach()
endif()
