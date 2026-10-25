# Checks which editor stack a built plug-in module carries.
#
#   cmake -DNM=<nm> -DMODULE=<binary> -DEXPECT=lite|full -P check_plugin_module_editor_stack.cmake
#
# lite: no Skia, Dawn, Tint or macOS GPU host symbols (a LITE_EDITOR plug-in).
# full: all of them present, which is also what proves the patterns below
#       still match the symbols they are meant to find.
# The check refuses a module with no Pulp symbols at all, so an unreadable or
# wrong file cannot pass as "lite".

if(NOT NM OR NOT MODULE OR NOT EXPECT)
    message(FATAL_ERROR "usage: -DNM=<nm> -DMODULE=<binary> -DEXPECT=lite|full")
endif()
if(NOT EXISTS "${MODULE}")
    message(FATAL_ERROR "module not found: ${MODULE}")
endif()

execute_process(COMMAND "${NM}" "${MODULE}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm failed for ${MODULE}: ${err}")
endif()

string(REGEX MATCHALL "N4pulp" pulp_syms "${out}")
list(LENGTH pulp_syms pulp_count)
if(pulp_count EQUAL 0)
    message(FATAL_ERROR "${MODULE} has no Pulp symbols; this is not a Pulp plug-in module")
endif()

set(families
    "Skia|8SkCanvas|9SkSurface|9SkFontMgr|10SkTypeface"
    "Dawn|4dawn6native"
    "Tint|4tint"
    "GPU host|MacGpuPluginViewHost")
set(present "")
set(absent "")
foreach(entry IN LISTS families)
    string(REPLACE "|" ";" parts "${entry}")
    list(POP_FRONT parts label)
    string(REPLACE ";" "|" pattern "${parts}")
    string(REGEX MATCHALL "(${pattern})" hits "${out}")
    list(LENGTH hits n)
    if(n GREATER 0)
        list(APPEND present "${label} (${n})")
    else()
        list(APPEND absent "${label}")
    endif()
endforeach()

if(EXPECT STREQUAL "lite" AND present)
    message(FATAL_ERROR "${MODULE} should carry the lite editor stack but links: ${present}")
elseif(EXPECT STREQUAL "full" AND absent)
    message(FATAL_ERROR "${MODULE} should carry the full editor stack but lacks: ${absent}")
elseif(NOT EXPECT MATCHES "^(lite|full)$")
    message(FATAL_ERROR "EXPECT must be lite or full, not ${EXPECT}")
endif()
message(STATUS "${MODULE}: ${EXPECT} editor stack (${pulp_count} Pulp symbols; present: ${present})")
