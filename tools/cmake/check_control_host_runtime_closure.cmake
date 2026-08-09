cmake_minimum_required(VERSION 3.24)

if(NOT APPLE)
    return()
endif()
if(NOT DEFINED ARTIFACT OR NOT EXISTS "${ARTIFACT}")
    message(FATAL_ERROR "control host runtime-closure check requires ARTIFACT")
endif()
execute_process(COMMAND /usr/bin/otool -L "${ARTIFACT}"
    RESULT_VARIABLE _otool_result
    OUTPUT_VARIABLE _dependencies
    ERROR_VARIABLE _otool_error)
if(NOT _otool_result EQUAL 0)
    message(FATAL_ERROR "could not inspect control host dependencies: ${_otool_error}")
endif()
if(_dependencies MATCHES "[\n\r][ \t]+@(rpath|loader_path)/")
    message(FATAL_ERROR
        "canonical author control hosts currently require a self-contained processor runtime; "
        "a non-system dynamic dependency remains in ${ARTIFACT}. Signed/hash-pinned runtime "
        "dependency closure is a launch blocker, so this artifact fails closed.")
endif()
