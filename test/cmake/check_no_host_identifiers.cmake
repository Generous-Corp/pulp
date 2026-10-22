#[[
Fail when a file Pulp installs into every SDK names a build host, a local
path or a user name. Files under share/pulp/ship (the signing preflight and
the default entitlements) are copied into each consumer's SDK, so a comment
naming a fleet machine or a home directory would ship with every release.

Inputs (via -D):
  SCAN_DIR    a directory whose regular files are all scanned, or
  SCAN_FILES  a ;-separated list of files to scan.
At least one file must be scanned; an empty set is an error, not a pass.
]]
cmake_minimum_required(VERSION 3.20)

set(_files "")
if(DEFINED SCAN_DIR)
    if(NOT IS_DIRECTORY "${SCAN_DIR}")
        message(FATAL_ERROR "SCAN_DIR is not a directory: ${SCAN_DIR}")
    endif()
    file(GLOB_RECURSE _dir_files LIST_DIRECTORIES false "${SCAN_DIR}/*")
    list(APPEND _files ${_dir_files})
endif()
if(DEFINED SCAN_FILES)
    list(APPEND _files ${SCAN_FILES})
endif()
list(LENGTH _files _file_count)
if(_file_count EQUAL 0)
    message(FATAL_ERROR "no files to scan (set SCAN_DIR or SCAN_FILES)")
endif()

# Short host aliases only count as whole words; the rest are distinctive.
set(_word_start "(^|[^A-Za-z0-9_.-])")
# A following "." ends the word unless a digit follows it ("m1.5" is a number).
set(_word_end "([^A-Za-z0-9_.-]|[.][^0-9]|[.]?$)")
set(_patterns
    "${_word_start}(m1|m3|m5|m5s|m5studio|macmini|macpro|macstudio)${_word_end}"
    "Daniels-[A-Za-z]"
    "danielraffel"
    "/Users/[A-Za-z]"
    "/home/[A-Za-z]"
    "/Volumes/[A-Za-z]"
    "[A-Za-z]:\\\\Users\\\\")

set(_problems "")
foreach(_file IN LISTS _files)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "file to scan does not exist: ${_file}")
    endif()
    # Split on newlines ourselves: file(STRINGS) would also split a line at
    # every `;`, which shell scripts are full of, and misreport line numbers.
    file(READ "${_file}" _text)
    string(REPLACE ";" "<semicolon>" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")
    set(_line_number 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _line_number "${_line_number} + 1")
        foreach(_pattern IN LISTS _patterns)
            if(_line MATCHES "${_pattern}")
                string(REPLACE "<semicolon>" ";" _shown "${_line}")
                string(APPEND _problems "  ${_file}:${_line_number}: ${_shown}\n")
                break()
            endif()
        endforeach()
    endforeach()
endforeach()

if(NOT _problems STREQUAL "")
    message(FATAL_ERROR
        "files installed into every SDK name a host, a local path or a user:\n"
        "${_problems}")
endif()
message(STATUS "scanned ${_file_count} file(s); no host, path or user names")
