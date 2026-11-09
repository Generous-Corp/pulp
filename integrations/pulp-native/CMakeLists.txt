# pulp-native: a native surface (a child NSView backed by a CAMetalLayer) for an
# editor that brings its own renderer. It links Pulp's platform layer and system
# frameworks only: no view, canvas, render or GPU-runtime target, and no Skia,
# Dawn, Vellum or RmlUi.

add_library(pulp-native STATIC
    src/metal_surface_mac.mm)
add_library(pulp::native ALIAS pulp-native)

target_include_directories(pulp-native PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_options(pulp-native PRIVATE -fobjc-arc)
target_compile_features(pulp-native PUBLIC cxx_std_20)
target_link_libraries(pulp-native
    PUBLIC
        pulp::platform
    PRIVATE
        "-framework AppKit"
        "-framework Metal"
        "-framework QuartzCore")
