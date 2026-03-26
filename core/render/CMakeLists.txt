add_library(pulp-render)
add_library(pulp::render ALIAS pulp-render)

target_include_directories(pulp-render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-render PRIVATE
    src/gpu_surface_dawn.cpp
    src/skia_surface.cpp
)

# iOS Metal surface for Dawn rendering
if(PULP_IOS)
    target_sources(pulp-render PRIVATE
        src/metal_surface_ios.mm
    )
    target_link_libraries(pulp-render PRIVATE
        "-framework UIKit"
        "-framework Metal"
        "-framework QuartzCore"
    )
endif()

target_link_libraries(pulp-render PUBLIC pulp::runtime pulp::canvas)

# Link WebGPU if available
if(PULP_HAS_WEBGPU)
    target_link_libraries(pulp-render PUBLIC webgpu)
    target_compile_definitions(pulp-render PUBLIC PULP_HAS_WEBGPU=1)
endif()

# Link Skia Graphite if available
if(PULP_HAS_SKIA)
    target_link_libraries(pulp-render PUBLIC skia::skia)
    if(TARGET skia::dawn)
        target_link_libraries(pulp-render PUBLIC skia::dawn)
    endif()
    target_compile_definitions(pulp-render PUBLIC PULP_HAS_SKIA=1)
    target_include_directories(pulp-render PRIVATE ${SKIA_INCLUDE_DIRS})

    # Dawn headers from Skia's bundled Dawn
    if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
        target_include_directories(pulp-render PRIVATE "${SKIA_INCLUDE_DIRS}/dawn")
    endif()
endif()
