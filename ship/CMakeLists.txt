add_library(pulp-ship)
add_library(pulp::ship ALIAS pulp-ship)

target_include_directories(pulp-ship
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Cross-platform sources
target_sources(pulp-ship PRIVATE
    src/appcast.cpp
)

if(APPLE)
    target_sources(pulp-ship PRIVATE
        platform/mac/codesign_mac.mm
    )
    target_link_libraries(pulp-ship PRIVATE "-framework Security")
elseif(WIN32)
    target_sources(pulp-ship PRIVATE
        platform/win/codesign_win.cpp
    )
elseif(UNIX)
    target_sources(pulp-ship PRIVATE
        platform/linux/package_linux.cpp
    )
endif()
