# pulp-native: native surfaces for an editor that brings its own renderer. On
# macOS a child NSView backed by a CAMetalLayer, plus an offscreen OpenGL
# surface for tests and capture; on Windows a child window with a WGL context;
# on Linux a child window with a GLX context. It links Pulp's platform layer and system
# frameworks only: no view, canvas, render or GPU-runtime target, and no Skia,
# Dawn, Vellum or RmlUi.

add_library(pulp-native STATIC src/gl_surface.cpp src/input.cpp src/x11_translate.cpp)
if(APPLE)
    target_sources(pulp-native PRIVATE
        src/frame_pump_mac.cpp
        src/gl_surface_mac.cpp
        src/input_mac.mm
        src/metal_surface_mac.mm
        src/surface_input_mac.mm)
elseif(WIN32)
    target_sources(pulp-native PRIVATE src/gl_surface_win32.cpp)
else()
    # Linux: the GLX backend needs Xlib, its XKB header and libGL. Without
    # them the package still builds, with a GlSurface that has no windowed
    # backend (create() returns null).
    find_package(X11 QUIET)
    find_path(PULP_NATIVE_GLX_INCLUDE_DIR GL/glx.h)
    find_path(PULP_NATIVE_XKB_INCLUDE_DIR X11/XKBlib.h)
    find_library(PULP_NATIVE_GL_LIBRARY GL)
    if(X11_FOUND AND PULP_NATIVE_GLX_INCLUDE_DIR AND PULP_NATIVE_XKB_INCLUDE_DIR
       AND PULP_NATIVE_GL_LIBRARY)
        set(PULP_NATIVE_HAS_X11 TRUE)
        set(PULP_NATIVE_HAS_X11 TRUE CACHE INTERNAL "pulp-native builds its X11 GLX backend")
        target_sources(pulp-native PRIVATE src/gl_surface_x11.cpp)
    else()
        set(PULP_NATIVE_HAS_X11 FALSE)
        set(PULP_NATIVE_HAS_X11 FALSE CACHE INTERNAL "pulp-native builds its X11 GLX backend")
        if(PULP_NATIVE_REQUIRE_X11)
            message(FATAL_ERROR
                "PULP_NATIVE_REQUIRE_X11=ON but the X11 GLX backend cannot build: "
                "needs libX11, X11/XKBlib.h, GL/glx.h and libGL (libx11-dev, libgl-dev)")
        endif()
        message(STATUS "pulp-native: no X11/GLX development files; GlSurface has no windowed backend")
        target_sources(pulp-native PRIVATE src/gl_surface_none.cpp)
    endif()
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
elseif(PULP_NATIVE_HAS_X11)
    # Plain library names, so the installed archive does not carry this
    # machine's paths; a consumer needs the same development files.
    target_link_libraries(pulp-native PRIVATE X11 GL)
    target_include_directories(pulp-native PRIVATE
        ${X11_INCLUDE_DIR} ${PULP_NATIVE_GLX_INCLUDE_DIR} ${PULP_NATIVE_XKB_INCLUDE_DIR})
endif()
