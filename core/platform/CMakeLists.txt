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
    src/registry.cpp
)

# Platform-specific UI services and child process
if(APPLE)
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
elseif(WIN32)
    target_sources(pulp-platform PRIVATE
        platform/win/clipboard_win.cpp
        platform/win/child_process_win.cpp
    )
elseif(UNIX AND NOT APPLE)
    target_sources(pulp-platform PRIVATE
        platform/linux/clipboard_linux.cpp
        platform/posix/child_process_posix.cpp
    )
endif()
