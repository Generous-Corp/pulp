# Fails when a built Linux plug-in module has an unresolved symbol or does not
# name libfontconfig as a needed library.
#
#   cmake -DLDD=<ldd> -DREADELF=<readelf> -DMODULE=<.so> [-DEXPECT=clean|unresolved] \
#         -P check_plugin_module_no_unresolved.cmake
#
# `ldd -r` reports every undefined symbol the loader cannot bind. A plug-in is
# loaded by a host that may not have fontconfig in its process, so the module
# has to resolve Skia's Fc* calls through its own dependencies. EXPECT defaults
# to clean; EXPECT=unresolved inverts the verdict and serves as the negative
# control (a module linked without the fix must report the Fc* names).
#
# -DNM=<nm> -DREQUIRE_FONTCONFIG=ON makes the clean verdict non-vacuous: the
# module must reference Fc* functions (so it really carries Skia's font code)
# AND name libfontconfig as needed. A module that references none would pass
# the unresolved-symbol probe whether or not the fix exists.

if(NOT LDD OR NOT READELF OR NOT MODULE)
    message(FATAL_ERROR "usage: -DLDD=<ldd> -DREADELF=<readelf> -DMODULE=<so> [-DEXPECT=clean|unresolved]")
endif()
if(NOT EXISTS "${MODULE}")
    message(FATAL_ERROR "module not found: ${MODULE}")
endif()
if(NOT EXPECT)
    set(EXPECT clean)
endif()

execute_process(COMMAND "${LDD}" -r "${MODULE}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(all "${out}\n${err}")
if(EXPECT STREQUAL "clean" AND NOT "${rc}" STREQUAL "0")
    message(FATAL_ERROR "ldd -r failed for ${MODULE} (${rc}):\n${all}")
endif()
if(EXPECT STREQUAL "clean" AND NOT all MATCHES "libc\\.so|libstdc\\+\\+\\.so|statically linked")
    message(FATAL_ERROR "ldd -r printed no dependency list for ${MODULE}; the probe is not reading it:\n${all}")
endif()

string(REGEX MATCHALL "undefined symbol: [^\n ]+" unresolved "${all}")
list(LENGTH unresolved n)
execute_process(COMMAND "${READELF}" -d "${MODULE}"
    RESULT_VARIABLE rc2 OUTPUT_VARIABLE dyn)
# A dependency-free control is still a dynamic ELF module. With --as-needed
# it can have no DT_NEEDED entries; DT_NULL terminates every dynamic section.
if(NOT rc2 EQUAL 0 OR NOT dyn MATCHES "\\(NULL\\)")
    message(FATAL_ERROR "readelf -d could not read the dynamic section of ${MODULE}")
endif()
set(names_fontconfig FALSE)
if(dyn MATCHES "\\(NEEDED\\)[^\n]*libfontconfig")
    set(names_fontconfig TRUE)
endif()

if(REQUIRE_FONTCONFIG AND EXPECT STREQUAL "clean")
    if(NOT NM)
        message(FATAL_ERROR "REQUIRE_FONTCONFIG needs -DNM=<nm>")
    endif()
    execute_process(COMMAND "${NM}" -D --undefined-only "${MODULE}"
        RESULT_VARIABLE rc3 OUTPUT_VARIABLE undef)
    if(NOT rc3 EQUAL 0)
        message(FATAL_ERROR "nm failed for ${MODULE}")
    endif()
    string(REGEX MATCHALL "U Fc[A-Za-z]+" fc_refs "${undef}")
    list(LENGTH fc_refs fc_count)
    if(fc_count EQUAL 0)
        message(FATAL_ERROR "${MODULE} references no Fc* function, so this check cannot show the fontconfig fix; "
            "the fixture no longer carries Skia's font code")
    endif()
    if(NOT names_fontconfig)
        message(FATAL_ERROR "${MODULE} references ${fc_count} Fc* functions but does not name libfontconfig as needed")
    endif()
endif()

if(EXPECT STREQUAL "clean")
    if(n GREATER 0)
        list(SUBLIST unresolved 0 8 sample)
        message(FATAL_ERROR "${MODULE}: ${n} unresolved symbols, e.g. ${sample}. "
            "fontconfig named as needed: ${names_fontconfig}")
    endif()
else()
    if(n EQUAL 0)
        message(FATAL_ERROR "${MODULE}: expected unresolved symbols but found none; the probe cannot see the defect")
    endif()
    # ldd -r exits nonzero when it cannot bind a symbol. Text alone (from a
    # malformed or failed invocation) must not satisfy the negative control.
    if("${rc}" STREQUAL "0")
        message(FATAL_ERROR "${MODULE}: ldd -r reported undefined symbols but exited 0; "
            "the unresolved verdict needs a nonzero ldd exit status:\n${all}")
    endif()
endif()
message(STATUS "${MODULE}: ${n} unresolved symbols; libfontconfig needed: ${names_fontconfig}")
