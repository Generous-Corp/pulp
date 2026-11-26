# Checks a link closure written by pulp_link_closure (one target name per line).
#   cmake -DCLOSURE_FILE=<file> -DREQUIRED=a,b -DFORBIDDEN=c,d \
#         -DWHAT=<label> -P check_link_closure.cmake
# Fails when a required target is missing or a forbidden one is present. A
# closure without its required targets means the walk measured nothing, which
# would pass every absence check, so REQUIRED must name at least one.
if(NOT CLOSURE_FILE OR NOT EXISTS "${CLOSURE_FILE}")
    message(FATAL_ERROR "link closure check: CLOSURE_FILE is missing: '${CLOSURE_FILE}'")
endif()
string(REPLACE "," ";" REQUIRED "${REQUIRED}")
string(REPLACE "," ";" FORBIDDEN "${FORBIDDEN}")
if(NOT REQUIRED)
    message(FATAL_ERROR "link closure check: REQUIRED must name at least one target")
endif()
file(STRINGS "${CLOSURE_FILE}" _closure)
foreach(_target IN LISTS REQUIRED)
    if(NOT "${_target}" IN_LIST _closure)
        message(FATAL_ERROR "link closure check: ${WHAT} must reach ${_target}")
    endif()
endforeach()
foreach(_target IN LISTS FORBIDDEN)
    if("${_target}" IN_LIST _closure)
        message(FATAL_ERROR "link closure check: ${WHAT} reaches ${_target}")
    endif()
endforeach()
list(LENGTH _closure _count)
message(STATUS "link closure check: ${WHAT}: ${_count} targets, none forbidden")
