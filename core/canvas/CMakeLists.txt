add_library(pulp-canvas)
add_library(pulp::canvas ALIAS pulp-canvas)

target_include_directories(pulp-canvas
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-canvas PRIVATE
    src/recording_canvas.cpp
    src/svg.cpp
    src/effects.cpp
)

# nanosvg (zlib license) for SVG loading
target_include_directories(pulp-canvas PRIVATE
    ${CMAKE_SOURCE_DIR}/external/nanosvg
)

# Platform-specific rendering backends
if(APPLE)
    target_sources(pulp-canvas PRIVATE
        platform/mac/cg_canvas.mm
    )
    target_link_libraries(pulp-canvas PRIVATE
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework Foundation"
    )
endif()

target_link_libraries(pulp-canvas PUBLIC pulp::runtime)

# Skia Graphite GPU rendering backend
if(PULP_HAS_SKIA)
    target_sources(pulp-canvas PRIVATE
        src/skia_canvas.cpp
    )
    target_link_libraries(pulp-canvas PUBLIC skia::skia)
    if(TARGET skia::dawn)
        target_link_libraries(pulp-canvas PUBLIC skia::dawn)
    endif()
    target_compile_definitions(pulp-canvas PUBLIC PULP_HAS_SKIA=1)
    target_include_directories(pulp-canvas PRIVATE ${SKIA_INCLUDE_DIRS})

    # Platform font managers for Skia text rendering
    if(WIN32)
        target_link_libraries(pulp-canvas PRIVATE dwrite)
    elseif(UNIX AND NOT APPLE)
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(FONTCONFIG fontconfig)
            if(FONTCONFIG_FOUND)
                target_include_directories(pulp-canvas PRIVATE ${FONTCONFIG_INCLUDE_DIRS})
                target_link_libraries(pulp-canvas PRIVATE ${FONTCONFIG_LIBRARIES})
            endif()
        endif()
    endif()
endif()
