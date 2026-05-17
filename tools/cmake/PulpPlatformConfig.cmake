# PulpPlatformConfig.cmake — platform detection variables for find_package(Pulp).
#
# Extracted from PulpConfig.cmake.in in the 2026-05 B3 refactor.
# Sets PULP_APPLE / PULP_IOS / PULP_MACOS / PULP_WINDOWS / PULP_LINUX
# and PULP_PLATFORM (one of "ios" / "mac" / "win" / "linux").
#
# Called once from PulpConfig.cmake at find_package() time.

if(APPLE)
    set(PULP_APPLE TRUE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(PULP_IOS TRUE)
        set(PULP_PLATFORM "ios")
    else()
        set(PULP_MACOS TRUE)
        set(PULP_PLATFORM "mac")
    endif()
elseif(WIN32)
    set(PULP_WINDOWS TRUE)
    set(PULP_PLATFORM "win")
elseif(UNIX)
    set(PULP_LINUX TRUE)
    set(PULP_PLATFORM "linux")
endif()
