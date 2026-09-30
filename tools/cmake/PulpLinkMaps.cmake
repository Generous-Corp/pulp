# Record which static-archive members each executable's link pulled.
#
# With PULP_RECORD_LINK_MAPS=ON (the macOS gate turns it on), every link runs
# through tools/ci/link-members-launcher.sh. It adds one `-Wl,-map,<file>`,
# exits with the linker's status, and for an executable keeps only the map's
# object list and the link arguments under `<build>/link-members/`, deleting
# the multi-megabyte map. The map is a side output: the linked bytes are the
# same with and without it. tools/ci/link_members.py collects the records for
# the per-job reuse record; nothing in the build reads them.
#
# Included before any target is created, because a target copies the
# launcher when it is defined. An existing launcher is left alone. Apple
# linkers only (ld64 and ld-prime both take -map).

option(PULP_RECORD_LINK_MAPS
    "Record each executable's pulled archive members from a linker map (macOS)" OFF)

if(PULP_RECORD_LINK_MAPS)
    if(NOT APPLE)
        message(STATUS "PULP_RECORD_LINK_MAPS: link maps need an Apple linker; not recording")
    else()
        set(_pulp_link_members_launcher
            "/bin/sh;${CMAKE_CURRENT_LIST_DIR}/../ci/link-members-launcher.sh;${CMAKE_BINARY_DIR}")
        foreach(_pulp_lang C CXX OBJC OBJCXX)
            if(NOT CMAKE_${_pulp_lang}_LINKER_LAUNCHER)
                set(CMAKE_${_pulp_lang}_LINKER_LAUNCHER "${_pulp_link_members_launcher}")
            endif()
        endforeach()
        message(STATUS "PULP_RECORD_LINK_MAPS: recording pulled archive members into ${CMAKE_BINARY_DIR}/link-members")
    endif()
endif()
