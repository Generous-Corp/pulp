# rmlui-native: RmlUi on a pulp-native Metal surface. It brings no renderer
# and no RmlUi build of its own: RmlUi comes from Vellum's installed
# VellumRmlUi::Native, and the caller supplies an Rml::RenderInterface.

find_package(VellumRmlUiNative CONFIG REQUIRED COMPONENTS Native Metal)

# The adapter needs the density ratio (dp lengths follow the backing scale),
# NativeContext::warm() (the first frame is prepared at attach) and the
# renderer trim() (a hidden editor hands its render targets back). Vellum
# reports them through the API macros in native.hpp, defined from the commit
# 37777f3 of the Vellum repository on. An older SDK would silently draw `dp`
# at 1/scale of its size, so the configure fails instead of building without it.
include(CheckCXXSourceCompiles)
get_target_property(_vellum_rmlui_native_includes VellumRmlUi::Native
    INTERFACE_INCLUDE_DIRECTORIES)
set(CMAKE_REQUIRED_INCLUDES "${_vellum_rmlui_native_includes}")
set(_pulp_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
unset(PULP_VELLUM_RMLUI_API_OK CACHE)
check_cxx_source_compiles(
    "#include <vellum/rmlui/native.hpp>
     #if !defined(VELLUM_RMLUI_NATIVE_API_VERSION) || VELLUM_RMLUI_NATIVE_API_VERSION < 1
     #error \"the Vellum RmlUi SDK defines no API version\"
     #endif
     #define PULP_NEEDED (VELLUM_RMLUI_NATIVE_FEATURE_DENSITY_RATIO | VELLUM_RMLUI_NATIVE_FEATURE_WARM | VELLUM_RMLUI_NATIVE_FEATURE_RENDERER_TRIM)
     #if (VELLUM_RMLUI_NATIVE_FEATURES & PULP_NEEDED) != PULP_NEEDED
     #error \"the Vellum RmlUi SDK lacks a feature the adapter needs\"
     #endif
     int main() { return 0; }"
    PULP_VELLUM_RMLUI_API_OK)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_pulp_try_compile_target_type}")
unset(_pulp_try_compile_target_type)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT PULP_VELLUM_RMLUI_API_OK)
    message(FATAL_ERROR "rmlui-native: the Vellum RmlUi SDK on CMAKE_PREFIX_PATH is older than "
        "Vellum commit 37777f3 (no VELLUM_RMLUI_NATIVE_API_VERSION, or missing the density "
        "ratio, warm or renderer trim feature). Without the density ratio the adapter would "
        "draw `dp` lengths at 1/scale of their size on a high-density display. Use a Vellum "
        "RmlUi SDK built from the commit pinned as vellum-rmlui-sdk in tools/deps/manifest.json, "
        "or later.")
endif()

add_library(pulp-rmlui-native STATIC src/rmlui_surface_mac.mm)
add_library(pulp::rmlui-native ALIAS pulp-rmlui-native)
set_source_files_properties(src/rmlui_surface_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_include_directories(pulp-rmlui-native PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-rmlui-native PUBLIC cxx_std_20)
target_link_libraries(pulp-rmlui-native
    PUBLIC
        pulp::native
        VellumRmlUi::Native
    PRIVATE
        "-framework AppKit"
        "-framework Metal")

# Vellum's Metal Rml::RenderInterface on the surface, when the Vellum install
# was built with VELLUM_RMLUI_METAL=ON.
if(TARGET VellumRmlUi::Metal)
    add_library(pulp-rmlui-native-metal STATIC src/metal_renderer_mac.mm)
    add_library(pulp::rmlui-native-metal ALIAS pulp-rmlui-native-metal)
    set_source_files_properties(src/metal_renderer_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(pulp-rmlui-native-metal
        PUBLIC
            pulp::rmlui-native
            VellumRmlUi::Metal
        PRIVATE
            "-framework Metal")
    message(STATUS "rmlui-native: VellumRmlUi::Metal found; building pulp::rmlui-native-metal")
else()
    message(STATUS "rmlui-native: VellumRmlUi::Metal not in this Vellum install; "
                   "pulp::rmlui-native-metal is not built")
endif()
