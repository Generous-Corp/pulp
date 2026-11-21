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
if(NOT all MATCHES "libc\\.so|libstdc\\+\\+\\.so|statically linked")
    message(FATAL_ERROR "ldd -r printed no dependency list for ${MODULE}; the probe is not reading it:\n${all}")
endif()

string(REGEX MATCHALL "undefined symbol: [^\n ]+" unresolved "${all}")
list(LENGTH unresolved n)
execute_process(COMMAND "${READELF}" -d "${MODULE}"
    RESULT_VARIABLE rc2 OUTPUT_VARIABLE dyn)
if(NOT rc2 EQUAL 0 OR NOT dyn MATCHES "NEEDED")
    message(FATAL_ERROR "readelf -d could not read the dynamic section of ${MODULE}")
endif()
set(names_fontconfig FALSE)
if(dyn MATCHES "\\(NEEDED\\)[^\n]*libfontconfig")
    set(names_fontconfig TRUE)
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
endif()
message(STATUS "${MODULE}: ${n} unresolved symbols; libfontconfig needed: ${names_fontconfig}")
