include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpCppStringChunks.cmake")

if(PULP_LITERAL_NEGATIVE_PROBE)
    string(REPEAT "x" 20000 _oversized_literal)
    pulp_require_portable_cpp_literal("${_oversized_literal}" "negative control")
    message(FATAL_ERROR "negative control unexpectedly accepted")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DPULP_LITERAL_NEGATIVE_PROBE=ON
        -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _negative_result
    OUTPUT_VARIABLE _negative_stdout
    ERROR_VARIABLE _negative_stderr)
if(_negative_result EQUAL 0)
    message(FATAL_ERROR "monolithic-literal negative control unexpectedly passed")
endif()
set(_negative_output "${_negative_stdout}${_negative_stderr}")
if(NOT _negative_output MATCHES "MSVC_PORTABLE_LITERAL_LIMIT_EXCEEDED")
    message(FATAL_ERROR "negative control failed for the wrong reason: ${_negative_output}")
endif()

string(REPEAT "0123456789abcdef" 1400 _payload)
string(PREPEND _payload "raw-delimiter-collision:)PULP0\";")
pulp_cpp_raw_string_chunks(
    "${_payload}" _rendered_chunks _chunk_count _max_chunk_bytes)
if(_chunk_count LESS 2)
    message(FATAL_ERROR "oversized payload was not split")
endif()
if(_max_chunk_bytes GREATER PULP_CPP_RAW_STRING_CHUNK_BYTES)
    message(FATAL_ERROR "emitted chunk exceeded configured chunk size")
endif()
if(_max_chunk_bytes GREATER PULP_MSVC_STRING_LITERAL_LIMIT_BYTES)
    message(FATAL_ERROR "emitted chunk exceeded MSVC literal limit")
endif()
if(NOT _rendered_chunks MATCHES "R\"P0_1\\(")
    message(FATAL_ERROR "raw-string delimiter collision was not avoided: ${_rendered_chunks}")
endif()

message(STATUS
    "portable C++ string chunks: ${_chunk_count} fragments, largest ${_max_chunk_bytes} bytes")
