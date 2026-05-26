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
    platform/android/package_android.cpp
)

target_link_libraries(pulp-ship PUBLIC pulp::platform)
# Ed25519 signing/verification for Sparkle appcasts (macOS plan item 7.3)
# routes through pulp::runtime::ed25519_* + base64 helpers.
target_link_libraries(pulp-ship PRIVATE pulp::runtime)

if(APPLE)
    target_sources(pulp-ship PRIVATE
        platform/mac/codesign_mac.mm
    )
    target_link_libraries(pulp-ship PRIVATE "-framework Security")
elseif(WIN32)
    target_sources(pulp-ship PRIVATE
        platform/win/codesign_win.cpp
        platform/win/installer_win.cpp
    )
elseif(UNIX)
    target_sources(pulp-ship PRIVATE
        platform/linux/package_linux.cpp
    )
endif()
