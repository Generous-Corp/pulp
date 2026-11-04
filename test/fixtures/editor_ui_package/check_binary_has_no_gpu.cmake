# Checks what a built binary actually carries, from its full symbol table
# (local symbols included, so a Dawn linked with hidden visibility still shows):
#
#   cmake -DNM=<nm> -DBINARY=<executable> -DEXPECT=absent|present
#       -P check_binary_has_no_gpu.cmake
#
# EXPECT=absent: the binary defines no Dawn (wgpuCreateInstance, dawn::native)
# and no MacGpuWindowHost. This is what a UI package plug-in's standalone must
# hold: its only Dawn may come from its UI package. A link-closure check reads
# target names; this reads the image, so a new view target, an imported Dawn
# or Skia target, a framework or a raw link flag cannot slip past it.
#
# EXPECT=present: the control. The binary must define Dawn, which shows the
# listing can see Dawn when it is there.
#
# Either way the binary must define `main`, the control that the listing is
# not empty.
cmake_minimum_required(VERSION 3.24)

execute_process(COMMAND "${NM}" "${BINARY}"
    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} ${BINARY} failed (${_rc}): ${_err}")
endif()

string(REPLACE "\n" ";" _lines "${_out}")
set(_has_main FALSE)
set(_dawn "")
set(_gpu_host "")
foreach(_line IN LISTS _lines)
    # "<address> <type> <name>"; undefined references carry type U and no
    # address. Only definitions count.
    if(NOT _line MATCHES "^[0-9a-fA-F]+ +([A-Za-z]) +(.+)$")
        continue()
    endif()
    set(_type "${CMAKE_MATCH_1}")
    set(_name "${CMAKE_MATCH_2}")
    if(_type STREQUAL "U" OR _type STREQUAL "u")
        continue()
    endif()
    if(_name MATCHES "^_?main$")
        set(_has_main TRUE)
    endif()
    if(_name MATCHES "^_?wgpuCreateInstance$" OR _name MATCHES "4dawn6native")
        list(APPEND _dawn "${_name}")
    endif()
    if(_name MATCHES "MacGpuWindowHost")
        list(APPEND _gpu_host "${_name}")
    endif()
endforeach()

if(NOT _has_main)
    message(FATAL_ERROR "${BINARY} defines no main: the symbol listing is blind")
endif()
list(LENGTH _dawn _dawn_count)
list(LENGTH _gpu_host _host_count)
if(EXPECT STREQUAL "absent")
    if(_dawn_count GREATER 0 OR _host_count GREATER 0)
        list(SUBLIST _dawn 0 5 _dawn_sample)
        list(SUBLIST _gpu_host 0 5 _host_sample)
        message(FATAL_ERROR
            "${BINARY} carries Pulp's GPU stack: ${_dawn_count} Dawn definition(s) "
            "(${_dawn_sample}) and ${_host_count} MacGpuWindowHost definition(s) (${_host_sample})")
    endif()
    message(STATUS "${BINARY}: no Dawn and no MacGpuWindowHost")
elseif(EXPECT STREQUAL "present")
    if(_dawn_count EQUAL 0)
        message(FATAL_ERROR "${BINARY} defines no Dawn, so the absence check is blind")
    endif()
    message(STATUS "${BINARY}: ${_dawn_count} Dawn definition(s), as the control expects")
else()
    message(FATAL_ERROR "EXPECT must be absent or present")
endif()
