# pulp_add_header_weight_test(NAME HEADER <path> USAGE <target> FORBID <regex>
#                             MAX_LINES <n> [DEFINITIONS ...])
# Registers a ctest that preprocesses HEADER on its own with USAGE's interface
# include directories and definitions, and fails when it pulls in a header
# matching FORBID or grows past MAX_LINES (see check_header_weight.cmake).
function(pulp_add_header_weight_test name)
    cmake_parse_arguments(PARSE_ARGV 1 _hw "" "HEADER;USAGE;FORBID;MAX_LINES" "DEFINITIONS;LABELS")
    # The usage requirements go through a response file the compiler reads,
    # so definitions with spaces or quotes survive.
    set(_includes "$<TARGET_PROPERTY:${_hw_USAGE},INTERFACE_INCLUDE_DIRECTORIES>")
    set(_defines "$<TARGET_PROPERTY:${_hw_USAGE},INTERFACE_COMPILE_DEFINITIONS>")
    set(_rsp "${CMAKE_CURRENT_BINARY_DIR}/header_weight/${name}.rsp")
    set(_extra "")
    foreach(_def IN LISTS _hw_DEFINITIONS)
        string(APPEND _extra "\"-D${_def}\"\n")
    endforeach()
    if(APPLE AND CMAKE_OSX_SYSROOT)
        string(APPEND _extra "-isysroot\n\"${CMAKE_OSX_SYSROOT}\"\n")
    endif()
    file(GENERATE OUTPUT "${_rsp}" CONTENT
"$<$<BOOL:${_includes}>:\"-I$<JOIN:${_includes},\"\n\"-I>\"\n>$<$<BOOL:${_defines}>:\"-D$<JOIN:${_defines},\"\n\"-D>\"\n>${_extra}")
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            "-DCOMPILER=${CMAKE_CXX_COMPILER}"
            "-DFLAGS=@${_rsp}"
            "-DHEADER=${_hw_HEADER}"
            "-DFORBID=${_hw_FORBID}"
            "-DMAX_LINES=${_hw_MAX_LINES}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/header_weight"
            -P "${PULP_ROOT_DIR}/test/support/check_header_weight.cmake")
    if(NOT _hw_LABELS)
        set(_hw_LABELS header-weight)
    endif()
    set_tests_properties(${name} PROPERTIES LABELS "${_hw_LABELS}")
endfunction()
