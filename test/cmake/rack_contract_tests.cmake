# The contract between the Rack generators and the app that watches them.
#
# The app learns a generation ended by matching the wording the generator
# printed on its way out. A message no rule matches reads as progress: the
# outcome stays `running`, the stage never resolves, and the app waits forever
# on a run that stopped minutes ago -- which is what a person sees as a spinner
# that never finishes.
#
# Registered here, next to the other pure-Python Rack harnesses, for the reason
# rack_portmap_tests.cmake gives: gating a check that needs no Rack SDK on
# PULP_HAS_RACK means it only ever runs on a Rack-enabled build, which for a
# check that guards a silent hang is the same as not having it. This one had
# been reporting 24 unmatched endings, correctly and with exit 1, while nothing
# ran it.
if(Python3_Interpreter_FOUND)
    # Reads both sides from source -- every `raise SystemExit(...)` in the two
    # generators against every rule in build_monitor.cpp, plus drive_app's own
    # list -- so neither side is a copy that can drift. It also fails on a rule
    # matching wording no generator prints any more, which is how a guard
    # quietly stops guarding.
    add_test(NAME rack-generator-endings
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_generator_endings.py)
    set_tests_properties(rack-generator-endings PROPERTIES
        LABELS "rack;contract"
        TIMEOUT 60)
endif()
