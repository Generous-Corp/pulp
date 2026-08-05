# Does a generated patch play what was written into it? (tools/rack/fidelity.py)
#
# Registered as pure Python, alongside the port-map harness and for the same
# reason: the checks below need no Rack SDK, and gating them on PULP_HAS_RACK
# would leave them running only in a Rack-enabled developer build.
#
# What runs here is the SHAPE of the harness -- the comparator that has to
# notice a clamped value, the pitch reader that must not find notes in
# silence, the instrumentation that must leave no path to a sound card. The
# three real launches (`--with-rack`) are deliberately not run in CI: they
# need Rack, a licence, four seconds of engine time and a Mac, and each one
# opens CoreAudio to enumerate devices. Run them by hand with
# `python3 tools/rack/test_fidelity.py --with-rack`.
if(Python3_Interpreter_FOUND)
    add_test(NAME rack-fidelity
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/rack/test_fidelity.py)
    set_tests_properties(rack-fidelity PROPERTIES
        LABELS "rack;patch"
        TIMEOUT 120)
endif()
