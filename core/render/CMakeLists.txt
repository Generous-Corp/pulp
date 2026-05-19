add_library(pulp-render)
add_library(pulp::render ALIAS pulp-render)

target_include_directories(pulp-render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-render PRIVATE
    src/gpu_surface_dawn.cpp
    src/gpu_compute.cpp
    src/skia_surface.cpp
    src/render_loop.cpp
    src/draco_decoder.cpp
    src/ktx2_decoder.cpp
    src/texture_atlas.cpp
)
# SDL3 surface is desktop-only (Android uses PulpSurfaceView via JNI)
if(NOT ANDROID)
    target_sources(pulp-render PRIVATE src/sdl3_surface.cpp)
endif()

# DRACO mesh decompression (optional)
if(PULP_HAS_DRACO)
    target_link_libraries(pulp-render PRIVATE draco)
    target_compile_definitions(pulp-render PRIVATE PULP_HAS_DRACO)
endif()

# Apple Metal surface helpers for Dawn rendering
if(PULP_MACOS OR (APPLE AND NOT PULP_IOS))
    target_sources(pulp-render PRIVATE
        src/metal_surface_mac.mm
    )
    target_link_libraries(pulp-render PRIVATE
        "-framework Cocoa"
        "-framework Metal"
        "-framework QuartzCore"
    )
elseif(PULP_IOS)
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

# SDL3 for cross-platform surface extraction
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-render PRIVATE
        $<BUILD_INTERFACE:SDL3::SDL3-static>
        $<INSTALL_INTERFACE:Pulp::sdl3-static>
    )
    target_compile_definitions(pulp-render PRIVATE PULP_HAS_SDL3=1)
endif()

# CVDisplayLink for macOS render loop
if(APPLE AND NOT PULP_IOS)
    target_link_libraries(pulp-render PRIVATE "-framework CoreVideo")
endif()

# Link WebGPU if available
# On Android, Dawn is bundled in Skia — no separate webgpu target needed
if(PULP_HAS_WEBGPU AND TARGET webgpu)
    target_link_libraries(pulp-render PUBLIC webgpu)
endif()
if(PULP_HAS_WEBGPU)
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
