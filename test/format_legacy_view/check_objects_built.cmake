# OBJECTS is a list of object files the build must have produced, each newer
# than every file in INPUTS. Fails when
# an object is missing or stale, so the test reports what the build actually
# compiled rather than passing on its own.
list(LENGTH OBJECTS _count)
if(_count EQUAL 0)
    message(FATAL_ERROR "no objects to check")
endif()
foreach(_object IN LISTS OBJECTS)
    if(NOT EXISTS "${_object}")
        message(FATAL_ERROR "not built: ${_object}")
    endif()
    foreach(_input IN LISTS INPUTS)
        if("${_input}" IS_NEWER_THAN "${_object}" AND NOT "${_input}" STREQUAL "${_object}")
            file(TIMESTAMP "${_input}" _in_time)
            file(TIMESTAMP "${_object}" _obj_time)
            if(NOT _in_time STREQUAL _obj_time)
                message(FATAL_ERROR "stale: ${_object} is older than ${_input}")
            endif()
        endif()
    endforeach()
endforeach()
# PRESENT: more objects that must exist, without a freshness check (each
# depends on its own sources, which the caller does not list).
foreach(_object IN LISTS PRESENT)
    if(NOT EXISTS "${_object}")
        message(FATAL_ERROR "not built: ${_object}")
    endif()
endforeach()
list(LENGTH PRESENT _present)
message(STATUS "${_count} object(s) built and current, ${_present} more present")
