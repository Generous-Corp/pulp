# Artifact check for the bundle-name quoting fixture. Every format built from a
# name with spaces and parentheses must have produced its binary, and on Apple
# its bundle must carry the files the POST_BUILD steps write.

if(NOT PLUGIN_NAME MATCHES "[ (]" OR NOT PLUGIN_NAME MATCHES "[)]")
    message(FATAL_ERROR
        "PLUGIN_NAME '${PLUGIN_NAME}' no longer contains spaces and parentheses, "
        "so this check no longer exercises shell quoting.")
endif()

set(_failures "")
set(_checked 0)

foreach(_fmt STANDALONE VST3 CLAP AU)
    if(NOT DEFINED ${_fmt}_FILE)
        continue()
    endif()
    math(EXPR _checked "${_checked} + 1")
    if(NOT EXISTS "${${_fmt}_FILE}")
        list(APPEND _failures "${_fmt}: binary missing at ${${_fmt}_FILE}")
    endif()
    if(NOT "${${_fmt}_FILE}" MATCHES "\\(dev\\)")
        list(APPEND _failures "${_fmt}: path does not carry the plug-in name: ${${_fmt}_FILE}")
    endif()
endforeach()

if(EXPECT_PKGINFO)
    foreach(_fmt VST3 CLAP AU)
        if(NOT DEFINED ${_fmt}_BUNDLE)
            continue()
        endif()
        set(_pkginfo "${${_fmt}_BUNDLE}/Contents/PkgInfo")
        if(NOT EXISTS "${_pkginfo}")
            list(APPEND _failures "${_fmt}: Contents/PkgInfo missing")
            continue()
        endif()
        file(READ "${_pkginfo}" _bytes HEX)
        # "BNDL????" plus a newline.
        if(NOT _bytes STREQUAL "424e444c3f3f3f3f0a")
            list(APPEND _failures "${_fmt}: Contents/PkgInfo holds ${_bytes}")
        endif()
    endforeach()
    if(DEFINED VST3_BUNDLE AND
       NOT EXISTS "${VST3_BUNDLE}/Contents/Resources/moduleinfo.json")
        list(APPEND _failures "VST3: Contents/Resources/moduleinfo.json missing")
    endif()
endif()

if(_checked LESS 1)
    message(FATAL_ERROR "No plug-in artifact was passed to the check.")
endif()

if(_failures)
    string(REPLACE ";" "\n  " _rendered "${_failures}")
    message(FATAL_ERROR "Bundles named '${PLUGIN_NAME}' are incomplete:\n  ${_rendered}")
endif()

message(STATUS "bundle_name_quoting_verified=true formats=${_checked}")
