# pulp-native: native surfaces for an editor that brings its own renderer. On
# macOS a child NSView backed by a CAMetalLayer, plus an offscreen OpenGL
# surface for tests and capture; on Windows a child window with a WGL context. It links Pulp's platform layer and system
# frameworks only: no view, canvas, render or GPU-runtime target, and no Skia,
# Dawn, Vellum or RmlUi.

add_library(pulp-native STATIC src/gl_surface.cpp)
if(APPLE)
    target_sources(pulp-native PRIVATE
        src/frame_pump_mac.cpp
        src/gl_surface_mac.cpp
        src/input_mac.mm
        src/metal_surface_mac.mm
        src/surface_input_mac.mm)
elseif(WIN32)
    target_sources(pulp-native PRIVATE src/gl_surface_win32.cpp)
endif()
add_library(pulp::native ALIAS pulp-native)

target_include_directories(pulp-native PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_source_files_properties(src/input_mac.mm src/metal_surface_mac.mm src/surface_input_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_compile_features(pulp-native PUBLIC cxx_std_20)
target_link_libraries(pulp-native PUBLIC pulp::platform)
if(APPLE)
    target_link_libraries(pulp-native PRIVATE
        "-framework AppKit"
        "-framework CoreVideo"
        "-framework Metal"
        "-framework OpenGL"
        "-framework QuartzCore")
elseif(WIN32)
    target_link_libraries(pulp-native PRIVATE opengl32 user32 gdi32)
endif()
