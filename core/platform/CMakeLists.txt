add_library(pulp-platform)
add_library(pulp::platform ALIAS pulp-platform)

target_include_directories(pulp-platform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-platform PRIVATE
    src/platform.cpp
    src/file_dialog_stub.cpp
    src/popup_menu_stub.cpp
    src/permissions.cpp
    src/registry.cpp
    src/clipboard_common.cpp
)

# Platform-specific UI services and child process
if(APPLE AND NOT IOS)
    target_sources(pulp-platform PRIVATE
        platform/mac/clipboard_mac.mm
        platform/mac/file_dialog_mac.mm
        platform/mac/popup_menu_mac.mm
        platform/posix/child_process_posix.cpp
    )
    target_link_libraries(pulp-platform PRIVATE
        "-framework Cocoa"
        "-framework UniformTypeIdentifiers"
    )
elseif(IOS)
    # iOS: UIKit-backed clipboard; posix_spawn works (addchdir_np is
    # gated inside child_process_posix.cpp via TargetConditionals).
    # File dialogs and popup menus fall through to the stubs.
    target_sources(pulp-platform PRIVATE
        platform/ios/clipboard_ios.mm
        platform/posix/child_process_posix.cpp
    )
    target_link_libraries(pulp-platform PRIVATE
        "-framework UIKit"
        "-framework UniformTypeIdentifiers"
    )
elseif(WIN32)
    target_sources(pulp-platform PRIVATE
        platform/win/clipboard_win.cpp
        platform/win/child_process_win.cpp
    )
elseif(ANDROID)
    # Android platform sources wired via PulpAndroid.cmake → pulp_wire_android_sources()
    target_sources(pulp-platform PRIVATE
        platform/posix/child_process_posix.cpp
    )
elseif(UNIX AND NOT APPLE)
    target_sources(pulp-platform PRIVATE
        platform/linux/clipboard_linux.cpp
        platform/posix/child_process_posix.cpp
    )
endif()
