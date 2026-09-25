# Source-only Python selftests routed to the build-free required lane.
#
# tools/ci/source_selftests.json lists registrations that read nothing but the
# checkout. This file labels each of them `source-selftest`. The required macOS
# gate excludes that label on gate events (tools/ci/ctest_gate_args.py), and the
# required `Enforce version & skill sync` context runs every manifest entry
# (tools/ci/source_selftests.py run), so each one still blocks a merge. Every
# other lane, including push to main and a local `ctest`, still runs them here.
#
# Included last so every listed registration already exists. A listed name that
# this configuration did not register is reported, not fatal: some registrations
# are platform- or option-dependent. source-selftest-lane-contract is what keeps
# the two lanes honest on the gate host.
if(NOT Python3_Interpreter_FOUND)
    return()
endif()

set(_pulp_source_selftest_manifest "${CMAKE_SOURCE_DIR}/tools/ci/source_selftests.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_pulp_source_selftest_manifest}")
file(READ "${_pulp_source_selftest_manifest}" _pulp_source_selftest_json)
string(JSON _pulp_source_selftest_count LENGTH "${_pulp_source_selftest_json}" tests)
math(EXPR _pulp_source_selftest_last "${_pulp_source_selftest_count} - 1")
set(_pulp_source_selftest_missing "")
foreach(_pulp_i RANGE ${_pulp_source_selftest_last})
    string(JSON _pulp_source_selftest_name GET
        "${_pulp_source_selftest_json}" tests ${_pulp_i} name)
    if(TEST "${_pulp_source_selftest_name}")
        set_property(TEST "${_pulp_source_selftest_name}"
            APPEND PROPERTY LABELS source-selftest)
    else()
        list(APPEND _pulp_source_selftest_missing "${_pulp_source_selftest_name}")
    endif()
endforeach()
if(_pulp_source_selftest_missing)
    message(STATUS
        "source-selftest lane: not registered in this configuration: "
        "${_pulp_source_selftest_missing}")
endif()

# The contract needs the configured tree, so it stays on the gate. On Apple,
# where the gate runs, every listed registration must exist.
set(_pulp_source_selftest_require_all "")
if(APPLE)
    set(_pulp_source_selftest_require_all --require-all)
endif()
add_test(NAME source-selftest-lane-contract
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/ci/source_selftests.py" check
        --build-dir "${CMAKE_BINARY_DIR}"
        --ctest "${CMAKE_CTEST_COMMAND}"
        ${_pulp_source_selftest_require_all})
add_test(NAME source-selftest-lane-selftest
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/ci/test_source_selftests.py")
