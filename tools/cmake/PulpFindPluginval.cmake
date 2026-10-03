# Find pluginval, which macOS ships as an app bundle (pluginval.app). CMake's
# app-bundle search returns the bundle's executable directory prefixed twice
# (".../pluginval.app/Contents/MacOS/Applications/pluginval.app/Contents/MacOS/
# pluginval", a path that does not exist) for every app-bundle lookup after the
# first in one configure, so a second plug-in's validation test registered no
# command. Search each bundle's Contents/MacOS as a plain directory instead, and
# drop a cached result that names no file so a configure poisoned that way heals.
function(_pulp_find_pluginval out_var)
    if(DEFINED CACHE{${out_var}} AND ${out_var} AND NOT EXISTS "${${out_var}}")
        unset(${out_var} CACHE)
    endif()
    set(_pulp_pluginval_hints "")
    foreach(_root IN LISTS CMAKE_APPBUNDLE_PATH CMAKE_SYSTEM_APPBUNDLE_PATH)
        list(APPEND _pulp_pluginval_hints "${_root}/pluginval.app/Contents/MacOS")
    endforeach()
    set(CMAKE_FIND_APPBUNDLE NEVER)
    find_program(${out_var} pluginval HINTS ${_pulp_pluginval_hints})
endfunction()
