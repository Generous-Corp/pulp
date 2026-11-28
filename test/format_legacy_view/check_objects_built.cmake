# OBJECTS is a list of object files the build must have produced from SOURCES
# (one source per object, same order), each newer than its source. Fails when
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
message(STATUS "${_count} object(s) built and current")
