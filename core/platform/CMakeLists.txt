add_library(pulp-platform)
add_library(pulp::platform ALIAS pulp-platform)

target_include_directories(pulp-platform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-platform PRIVATE
    src/platform.cpp
)

# Platform-specific UI services
if(APPLE)
    target_sources(pulp-platform PRIVATE
        platform/mac/clipboard_mac.mm
        platform/mac/file_dialog_mac.mm
        platform/mac/popup_menu_mac.mm
    )
    target_link_libraries(pulp-platform PRIVATE
        "-framework Cocoa"
        "-framework UniformTypeIdentifiers"
    )
elseif(UNIX AND NOT APPLE)
    target_sources(pulp-platform PRIVATE
        platform/linux/clipboard_linux.cpp
    )
endif()
