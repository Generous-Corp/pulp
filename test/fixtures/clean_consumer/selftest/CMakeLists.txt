# Self-test for the closure auditor, over synthetic targets.
#
# The installed-SDK fixture is slow and label-excluded from the fast gate, so
# the instrument it depends on is proven here instead: a handful of IMPORTED
# targets wired into known shapes, and an assertion for each answer the auditor
# can give. If this passes, a `clean` verdict from the real fixture is a
# measurement rather than a hope.
#
# The shape that matters most is the third one. CMake records a PRIVATE link to
# a static library as $<LINK_ONLY:dep> in INTERFACE_LINK_LIBRARIES, so it still
# lands on a consumer's link line. An auditor that reads the source text and
# honours the PRIVATE keyword under-reports exactly there, which is the single
# error that would make the whole contract check worthless. So it is asserted
# directly rather than assumed.

cmake_minimum_required(VERSION 3.24)
project(PulpCleanConsumerSelfTest LANGUAGES NONE)

if(NOT DEFINED PULP_CLEAN_CONSUMER_CLOSURE_MODULE)
    message(FATAL_ERROR "PULP_CLEAN_CONSUMER_CLOSURE_MODULE is required")
endif()
include("${PULP_CLEAN_CONSUMER_CLOSURE_MODULE}")

set(_failures "")
function(_expect label actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        set(_failures "${_failures};${label}: expected '${expected}', got '${actual}'"
            PARENT_SCOPE)
    endif()
endfunction()

# leaf <- mid <- root, plus a forbidden leaf reached only through $<LINK_ONLY:>.
add_library(selftest-leaf INTERFACE IMPORTED)
add_library(selftest-forbidden INTERFACE IMPORTED)
add_library(selftest-compile-only INTERFACE IMPORTED)
add_library(selftest-unrelated INTERFACE IMPORTED)

add_library(selftest-mid INTERFACE IMPORTED)
set_target_properties(selftest-mid PROPERTIES
    INTERFACE_LINK_LIBRARIES
        "selftest-leaf;\$<LINK_ONLY:selftest-forbidden>")

add_library(selftest-root INTERFACE IMPORTED)
set_target_properties(selftest-root PROPERTIES
    INTERFACE_LINK_LIBRARIES
        "selftest-mid;\$<COMPILE_ONLY:selftest-compile-only>;-framework Foo")

pulp_consumer_link_closure(_closure selftest-root)

# Reachability, including the LINK_ONLY edge that a text scan would miss.
foreach(_expected selftest-root selftest-mid selftest-leaf selftest-forbidden)
    if(NOT "${_expected}" IN_LIST _closure)
        _expect("closure contains ${_expected}" "absent" "present")
    endif()
endforeach()
# COMPILE_ONLY is a usage requirement that is deliberately not linked; keeping
# it would invent an edge.
if("selftest-compile-only" IN_LIST _closure)
    _expect("COMPILE_ONLY operand excluded" "present" "absent")
endif()
if("selftest-unrelated" IN_LIST _closure)
    _expect("unrelated target excluded" "present" "absent")
endif()

# violated: a realizing target exists and is reached.
pulp_consumer_classify_exclusion(demo _status _offenders
    CLOSURE ${_closure} REALIZED_BY selftest-forbidden)
_expect("reached-and-installed is violated" "${_status}" "violated")
_expect("violation names the offender" "${_offenders}" "selftest-forbidden")

# clean: a realizing target exists and is NOT reached.
pulp_consumer_classify_exclusion(demo _status _offenders
    CLOSURE ${_closure} REALIZED_BY selftest-unrelated)
_expect("installed-but-unreached is clean" "${_status}" "clean")

# unenforceable: no realizing target exists, so the exclusion proves nothing.
# This is the verdict that keeps a GPU-off SDK from scoring a free pass on
# every exclusion it could not possibly violate.
pulp_consumer_classify_exclusion(demo _status _offenders
    CLOSURE ${_closure} REALIZED_BY selftest-never-defined)
_expect("absent-from-SDK is unenforceable" "${_status}" "unenforceable")

# A group listing several realizing targets is unenforceable only when NONE of
# them exists; one present name is enough to make the check live.
pulp_consumer_classify_exclusion(demo _status _offenders
    CLOSURE ${_closure} REALIZED_BY selftest-never-defined selftest-unrelated)
_expect("one present name makes it live" "${_status}" "clean")

# The carrier walk has to name the target whose own interface holds the edge,
# not merely repeat that the dependency is in the closure.
pulp_consumer_edge_carriers(_carriers selftest-forbidden CLOSURE ${_closure})
_expect("carrier of the LINK_ONLY edge" "${_carriers}" "selftest-mid")

# The edge KIND decides what cutting it would take, so conflating the two ways
# a target can hold a dependency would make the report unactionable.
pulp_consumer_edge_kind(_kind selftest-mid selftest-forbidden)
_expect("LINK_ONLY edge is reported as such"
    "${_kind}" "INTERFACE_LINK_LIBRARIES/LINK_ONLY")
pulp_consumer_edge_kind(_kind selftest-mid selftest-leaf)
_expect("plain interface edge is reported as direct"
    "${_kind}" "INTERFACE_LINK_LIBRARIES/direct")
pulp_consumer_edge_kind(_kind selftest-mid selftest-unrelated)
_expect("absent edge is reported as none" "${_kind}" "none")

# The path is breadth-first, so it names the fewest edges to cut. Adding a
# longer alternative route must not change the answer.
add_library(selftest-detour-a INTERFACE IMPORTED)
set_target_properties(selftest-detour-a PROPERTIES
    INTERFACE_LINK_LIBRARIES "selftest-mid")
add_library(selftest-long-root INTERFACE IMPORTED)
set_target_properties(selftest-long-root PROPERTIES
    INTERFACE_LINK_LIBRARIES "selftest-mid;selftest-detour-a")

pulp_consumer_shortest_path(_path selftest-root selftest-forbidden)
_expect("shortest path names every hop"
    "${_path}" "selftest-root -> selftest-mid -> selftest-forbidden")
pulp_consumer_shortest_path(_path selftest-long-root selftest-forbidden)
_expect("a longer parallel route does not lengthen the path"
    "${_path}" "selftest-long-root -> selftest-mid -> selftest-forbidden")
pulp_consumer_shortest_path(_path selftest-root selftest-root)
_expect("path to self is the root alone" "${_path}" "selftest-root")
pulp_consumer_shortest_path(_path selftest-leaf selftest-forbidden)
_expect("unreachable goal yields no path" "${_path}" "")

# The assertion that actually pins "shortest" rather than merely "a path".
#
# A diamond whose two routes to the same goal have DIFFERENT lengths, with the
# long one listed first:
#
#   diamond-a -> long-b -> long-c -> long-d -> diamond-z   (4 hops, listed 1st)
#   diamond-a -> short-x -> diamond-z                      (2 hops, listed 2nd)
#
# A breadth-first search reports the 2-hop route. A depth-first one follows the
# first entry down and reports the 4-hop route, naming three edges nobody needs
# to cut. Only a genuinely shortest-path search passes, which is what makes
# this a positive discriminating control rather than a restatement of the
# implementation: swapping BFS for DFS is the realistic regression, and it
# fails here loudly.
#
# Both routes must be strictly longer than one hop, or the goal would be a
# level-one neighbour and found immediately whichever order the search used,
# which is exactly the flaw in a naive version of this fixture.
add_library(selftest-diamond-z INTERFACE IMPORTED)
add_library(selftest-short-x INTERFACE IMPORTED)
set_target_properties(selftest-short-x PROPERTIES
    INTERFACE_LINK_LIBRARIES "selftest-diamond-z")
add_library(selftest-long-d INTERFACE IMPORTED)
set_target_properties(selftest-long-d PROPERTIES
    INTERFACE_LINK_LIBRARIES "selftest-diamond-z")
add_library(selftest-long-c INTERFACE IMPORTED)
set_target_properties(selftest-long-c PROPERTIES
    INTERFACE_LINK_LIBRARIES "selftest-long-d")
add_library(selftest-long-b INTERFACE IMPORTED)
set_target_properties(selftest-long-b PROPERTIES
    INTERFACE_LINK_LIBRARIES "selftest-long-c")
add_library(selftest-diamond-a INTERFACE IMPORTED)
set_target_properties(selftest-diamond-a PROPERTIES
    INTERFACE_LINK_LIBRARIES "selftest-long-b;selftest-short-x")

pulp_consumer_shortest_path(_path selftest-diamond-a selftest-diamond-z)
_expect("the 2-hop route wins over a 4-hop route listed first"
    "${_path}" "selftest-diamond-a -> selftest-short-x -> selftest-diamond-z")

if(_failures)
    string(REPLACE ";" "\n  " _text "${_failures}")
    message(FATAL_ERROR "closure auditor self-test failed:${_text}")
endif()
message(STATUS "clean-consumer: closure auditor self-test ok")
