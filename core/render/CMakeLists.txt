# The GPU device layer sits below the 2D renderer as two targets.
#
#   pulp-gpu-device-api  INTERFACE. The six device headers (gpu_surface,
#                        gpu_compute, gpu_diagnostics, gpu_startup_report,
#                        gpu_render_time, bench/perf_counters). They expose
#                        opaque handles only, so this target names no Dawn,
#                        WebGPU or Skia dependency and no Pulp module.
#   pulp-gpu-device      The device implementation: the Dawn surface and
#                        compute TUs, GPU diagnostics, the startup report and
#                        the Apple Metal-layer helpers. It carries the same
#                        Skia/Dawn and webgpu links as pulp-render (native Dawn
#                        is libdawn_combined inside skia::skia; FindSkia defines
#                        no separate skia::dawn target).
#
# pulp-render links the API PUBLIC and the implementation PRIVATE, so every
# existing pulp::render consumer keeps the same headers, definitions and
# linked objects. pulp-gpu-audio links pulp::gpu-device directly and no longer
# reaches the 2D renderer. The headers stay in this include directory, so
# `#include <pulp/render/gpu_compute.hpp>` is unchanged for every consumer.
# Which headers belong to the API is checked by gpu-device-link-closure
# (test/cmake/render_gpu_device_closure_tests.cmake).
add_library(pulp-gpu-device-api INTERFACE)
add_library(pulp::gpu-device-api ALIAS pulp-gpu-device-api)
target_include_directories(pulp-gpu-device-api
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(pulp-gpu-device-api INTERFACE cxx_std_20)

add_library(pulp-gpu-device)
add_library(pulp::gpu-device ALIAS pulp-gpu-device)
target_sources(pulp-gpu-device PRIVATE
    src/gpu_diagnostics.cpp
    src/gpu_startup_report.cpp
    src/gpu_surface_dawn.cpp
    src/gpu_compute.cpp
)
target_link_libraries(pulp-gpu-device
    PUBLIC pulp::gpu-device-api
    PRIVATE pulp::runtime
)

# Apple Metal surface helpers for Dawn rendering
if(PULP_MACOS OR (APPLE AND NOT PULP_IOS))
    target_sources(pulp-gpu-device PRIVATE
        src/metal_surface_mac.mm
    )
    target_link_libraries(pulp-gpu-device PRIVATE
        "-framework Cocoa"
        "-framework Metal"
        "-framework QuartzCore"
    )
elseif(PULP_IOS)
    target_sources(pulp-gpu-device PRIVATE
        src/metal_surface_ios.mm
    )
    target_link_libraries(pulp-gpu-device PRIVATE
        "-framework UIKit"
        "-framework Metal"
        "-framework QuartzCore"
    )
endif()

add_library(pulp-render)
add_library(pulp::render ALIAS pulp-render)

target_include_directories(pulp-render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-render PRIVATE
    src/headless_surface.cpp
    src/skia_surface.cpp
    src/skp_capture.cpp
    src/render_loop.cpp
    src/draco_decoder.cpp
    src/ktx2_decoder.cpp
    src/texture_atlas.cpp
)
if(PULP_ENABLE_SCENE3D)
    target_sources(pulp-render PRIVATE
        src/renderer3d.cpp
    )
    target_compile_definitions(pulp-render PUBLIC PULP_ENABLE_SCENE3D=1)
    target_include_directories(pulp-render PRIVATE
        ${PULP_ROOT_DIR}/core/scene/include
    )

    add_executable(pulp-renderer3d-probe
        src/renderer3d_probe.cpp
    )
    target_link_libraries(pulp-renderer3d-probe PRIVATE
        pulp::render
        pulp::scene
    )

    add_executable(pulp-scene3d-inspect-native
        ${PULP_ROOT_DIR}/core/scene/src/scene3d_inspect.cpp
    )
    target_compile_definitions(pulp-scene3d-inspect-native PRIVATE
        PULP_SCENE3D_INSPECT_NATIVE_DRACO=1)
    target_link_libraries(pulp-scene3d-inspect-native PRIVATE
        pulp::render
        pulp::scene
    )
endif()
# SDL3 surface is desktop-only (Android uses PulpSurfaceView via JNI)
if(NOT ANDROID)
    target_sources(pulp-render PRIVATE src/sdl3_surface.cpp)
endif()

# VBlank-locked RenderLoop platform backends.
# iOS uses CADisplayLink; Android uses AChoreographer. macOS (CVDisplayLink),
# Windows (DwmFlush) and the Linux/timer fallback all live in render_loop.cpp.
if(PULP_IOS)
    target_sources(pulp-render PRIVATE src/render_loop_apple.mm)
elseif(ANDROID)
    target_sources(pulp-render PRIVATE src/render_loop_android.cpp)
endif()

# DRACO mesh decompression (optional)
if(PULP_HAS_DRACO)
    target_link_libraries(pulp-render PRIVATE draco::draco)
    target_compile_definitions(pulp-render PRIVATE PULP_HAS_DRACO)
    target_include_directories(pulp-render PRIVATE
        "${draco_SOURCE_DIR}/src"
        "${draco_BINARY_DIR}"
        "${CMAKE_BINARY_DIR}")
endif()

# The Metal surface helpers now live in pulp-gpu-device; pulp-render keeps the
# same framework links for its own Apple sources.
if(PULP_MACOS OR (APPLE AND NOT PULP_IOS))
    target_link_libraries(pulp-render PRIVATE
        "-framework Cocoa"
        "-framework Metal"
        "-framework QuartzCore"
    )
elseif(PULP_IOS)
    target_link_libraries(pulp-render PRIVATE
        "-framework UIKit"
        "-framework Metal"
        "-framework QuartzCore"
    )
endif()

target_link_libraries(pulp-render
    PUBLIC pulp::runtime pulp::canvas pulp::gpu-device-api
    PRIVATE pulp::gpu-device
)

# SDL3 for cross-platform surface extraction
if(PULP_HAS_SDL3)
    target_link_libraries(pulp-render PRIVATE
        $<BUILD_INTERFACE:SDL3::SDL3-static>
        $<INSTALL_INTERFACE:Pulp::sdl3-static>
    )
    target_compile_definitions(pulp-render PRIVATE PULP_HAS_SDL3=1)
endif()

# RenderLoop vblank sources:
#   macOS — CVDisplayLink (CoreVideo)
#   iOS   — CADisplayLink (QuartzCore, already linked above for Metal)
#   Win   — DwmFlush (dwmapi)
if(APPLE AND NOT PULP_IOS)
    target_link_libraries(pulp-render PRIVATE "-framework CoreVideo")
endif()
if(WIN32)
    target_link_libraries(pulp-render PRIVATE dwmapi)
endif()

# Link WebGPU if available
# On Android, Dawn is bundled in Skia — no separate webgpu target needed
# The Windows Skia archive includes Dawn and the Windows renderer uses
# dawn::native::* symbols that wgpu_native.dll does not export. Linking both
# implementations also defines every wgpu* C entry point twice. Prefer Skia's
# co-built static Dawn on Windows whenever that archive is present.
#
# pulp-gpu-device receives the same provider link, scope and definitions as
# pulp-render, so a consumer of either sees an identical Dawn/WebGPU surface.
foreach(_pulp_gpu_target IN ITEMS pulp-gpu-device pulp-render)
    if(PULP_HAS_WEBGPU AND TARGET webgpu
       AND NOT (WIN32 AND PULP_HAS_SKIA
               AND NOT DAWN_LIBRARY STREQUAL "DAWN_LIBRARY-NOTFOUND"))
        target_link_libraries(${_pulp_gpu_target} PUBLIC webgpu)
    endif()
    if(PULP_HAS_WEBGPU)
        target_compile_definitions(${_pulp_gpu_target} PUBLIC PULP_HAS_WEBGPU=1)
    endif()
endforeach()

# Link Skia Graphite if available
if(PULP_HAS_SKIA)
  foreach(_pulp_gpu_target IN ITEMS pulp-gpu-device pulp-render)
    target_link_libraries(${_pulp_gpu_target} PUBLIC skia::skia)
    if(TARGET skia::dawn)
        target_link_libraries(${_pulp_gpu_target} PUBLIC skia::dawn)
    endif()
    target_compile_definitions(${_pulp_gpu_target} PUBLIC PULP_HAS_SKIA=1)
    # The GPU-compute layer (gpu_compute.cpp / gpu_compute_pool.hpp) contains no
    # Skia — it needs Dawn. On native, Dawn arrives inside the Skia prebuilt
    # slice (skia::dawn + ${SKIA_INCLUDE_DIRS}/dawn below), so the two travel
    # together here; a browser build gets Dawn from Emscripten's emdawnwebgpu
    # port with no Skia at all and defines PULP_HAS_DAWN on its own.
    target_compile_definitions(${_pulp_gpu_target} PUBLIC PULP_HAS_DAWN=1)
    target_include_directories(${_pulp_gpu_target} PRIVATE ${SKIA_INCLUDE_DIRS})

    # Dawn headers from Skia's bundled Dawn
    if(EXISTS "${SKIA_INCLUDE_DIRS}/dawn")
        target_include_directories(${_pulp_gpu_target} PRIVATE "${SKIA_INCLUDE_DIRS}/dawn")
    endif()
  endforeach()
endif()
unset(_pulp_gpu_target)
