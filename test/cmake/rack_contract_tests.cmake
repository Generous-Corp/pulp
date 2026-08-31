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

    # Runs the generator's provider/output/rollback safety harness without a
    # Rack SDK.  Registering it gives changed-surface validation a literal,
    # inventory-bound test for edits to generate.py rather than forcing the
    # entire native corpus or relying on an out-of-band Python invocation.
    add_test(NAME rack-generator-safety
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_generate_safety.py)
    set_tests_properties(rack-generator-safety PROPERTIES
        LABELS "rack;contract"
        TIMEOUT 180)

    # Does the plugin we build actually LOAD where Rack loads it? Registered
    # here rather than behind PULP_HAS_RACK because the defect it guards --
    # a macOS plugin linked without libRack -- shipped for a month with five
    # green gates: every one of them dlopened the plugin inside a host that
    # had already linked libRack, where the failure cannot reproduce. It
    # skips loudly (exit 3) without the Rack SDK, and builds the real pack
    # rather than a stand-in. Longer timeout: it compiles the pack once.
    add_test(NAME rack-plugin-loads
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_rack_plugin_loads.py)
    set_tests_properties(rack-plugin-loads PROPERTIES
        LABELS "rack;contract"
        SKIP_RETURN_CODE 3
        TIMEOUT 300)

    # A cable into a CV input carries nothing while that input's depth control
    # sits at zero, and no structural check can tell that patch from a working
    # one -- so this is registered here rather than left to a Rack-enabled
    # build. Hermetic: it builds its own inventory and never touches the SDK.
    add_test(NAME rack-cv-depth
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_cv_depth.py)
    set_tests_properties(rack-cv-depth PROPERTIES
        LABELS "rack;contract"
        TIMEOUT 60)

    # A module saved with its run flag false makes no sound, and when it is
    # the master clock that silences the whole patch. Three of its cases
    # guard sign errors that INVERT a result rather than weaken it, so this
    # is registered rather than left to a Rack-enabled build. Hermetic.
    add_test(NAME rack-run-state
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_run_state.py)
    set_tests_properties(rack-run-state PROPERTIES
        LABELS "rack;contract"
        TIMEOUT 60)

    add_test(NAME rack-generation-eligibility
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_generation_eligibility.py)
    set_tests_properties(rack-generation-eligibility PROPERTIES
        LABELS "rack;contract"
        TIMEOUT 60)

    # What the gate SAYS when an input reads as inert, which decides whether a
    # generation can act on the failure or only observe it. Compiles two
    # fixtures against the real Rack SDK and runs the real gate, so it skips
    # cleanly where that SDK is absent rather than reporting a false pass.
    # Longer timeout than its siblings: it builds the gate twice.
    add_test(NAME rack-inert-input-cause
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_inert_input_cause.py)
    set_tests_properties(rack-inert-input-cause PROPERTIES
        LABELS "rack;contract"
        TIMEOUT 300)
endif()
