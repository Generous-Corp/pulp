# Fails when the graph editor's link closure (one target name per line in
# CLOSURE_FILE; pulp::host is a leaf, see CMakeLists.txt) reaches Pulp's own
# view stack or lacks the two edges it exists to join. Run with: cmake -DCLOSURE_FILE=<file> -P check_link_edges.cmake
if(NOT CLOSURE_FILE OR NOT EXISTS "${CLOSURE_FILE}")
    message(FATAL_ERROR "link-edge check: CLOSURE_FILE is missing: '${CLOSURE_FILE}'")
endif()
file(STRINGS "${CLOSURE_FILE}" _closure)
list(LENGTH _closure _count)
if(_count LESS 3)
    # A walk that found nothing would pass every absence check below; the
    # required edges are the control that the walk saw the package.
    message(FATAL_ERROR "link-edge check: only ${_count} targets in the closure, expected the package and its two edges")
endif()

foreach(_required IN ITEMS pulp::host Vellum::MirrorView_core)
    list(FIND _closure "${_required}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "link-edge check: the graph editor must link ${_required}")
    endif()
endforeach()

# Pulp's own UI targets. The editor draws through Vellum, so none of these may
# be reachable from it, directly or through pulp::host.
set(_forbidden
    pulp::view pulp::view-core pulp::view-native pulp::view-audio
    pulp::view-graph-editor pulp::canvas pulp::render)
foreach(_target IN LISTS _forbidden)
    list(FIND _closure "${_target}" _at)
    if(NOT _at EQUAL -1)
        message(FATAL_ERROR "link-edge check: the graph editor reaches ${_target}")
    endif()
endforeach()
message(STATUS "link-edge check: ${_count} targets, no Pulp view stack")
