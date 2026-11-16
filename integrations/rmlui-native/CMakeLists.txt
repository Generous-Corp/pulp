# rmlui-native: RmlUi on a pulp-native Metal surface. It brings no renderer
# and no RmlUi build of its own: RmlUi comes from Vellum's installed
# VellumRmlUiNative::Native, and the caller supplies an Rml::RenderInterface.

find_package(VellumRmlUiNative CONFIG REQUIRED COMPONENTS Native Metal)

# Older Vellum RmlUi SDKs do not expose the density setter. Detect the method
# at configure time so the adapter remains source-compatible with those SDKs.
include(CheckCXXSourceCompiles)
get_target_property(_vellum_rmlui_native_includes VellumRmlUiNative::Native
    INTERFACE_INCLUDE_DIRECTORIES)
set(CMAKE_REQUIRED_INCLUDES "${_vellum_rmlui_native_includes}")
set(_pulp_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_cxx_source_compiles(
    "#include <vellum/rmlui/native.hpp>
     int main() {
       vellum::rmlui::NativeContext* context = nullptr;
       (void) context->set_density_ratio(1.0f);
       return 0;
     }"
    PULP_VELLUM_RMLUI_HAS_DENSITY_RATIO)
# The first-frame preparation (NativeContext::warm) is newer than the density
# setter; older SDKs simply draw their first frame cold.
check_cxx_source_compiles(
    "#include <vellum/rmlui/native.hpp>
     int main() {
       vellum::rmlui::NativeContext* context = nullptr;
       context->warm();
       return 0;
     }"
    PULP_VELLUM_RMLUI_HAS_WARM)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_pulp_try_compile_target_type}")
unset(_pulp_try_compile_target_type)
unset(CMAKE_REQUIRED_INCLUDES)

add_library(pulp-rmlui-native STATIC src/rmlui_surface_mac.mm)
add_library(pulp::rmlui-native ALIAS pulp-rmlui-native)
set_source_files_properties(src/rmlui_surface_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_include_directories(pulp-rmlui-native PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-rmlui-native PUBLIC cxx_std_20)
target_link_libraries(pulp-rmlui-native
    PUBLIC
        pulp::native
        VellumRmlUiNative::Native
    PRIVATE
        "-framework AppKit"
        "-framework Metal")
if(PULP_VELLUM_RMLUI_HAS_DENSITY_RATIO)
    target_compile_definitions(pulp-rmlui-native PRIVATE PULP_VELLUM_RMLUI_HAS_DENSITY_RATIO)
endif()
if(PULP_VELLUM_RMLUI_HAS_WARM)
    target_compile_definitions(pulp-rmlui-native PRIVATE PULP_VELLUM_RMLUI_HAS_WARM)
endif()

# Vellum's Metal Rml::RenderInterface on the surface, when the Vellum install
# was built with VELLUM_RMLUI_METAL=ON.
if(TARGET VellumRmlUiNative::Metal)
    add_library(pulp-rmlui-native-metal STATIC src/metal_renderer_mac.mm)
    add_library(pulp::rmlui-native-metal ALIAS pulp-rmlui-native-metal)
    set_source_files_properties(src/metal_renderer_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(pulp-rmlui-native-metal
        PUBLIC
            pulp::rmlui-native
            VellumRmlUiNative::Metal
        PRIVATE
            "-framework Metal")
    message(STATUS "rmlui-native: VellumRmlUiNative::Metal found; building pulp::rmlui-native-metal")
else()
    message(STATUS "rmlui-native: VellumRmlUiNative::Metal not in this Vellum install; "
                   "pulp::rmlui-native-metal is not built")
endif()
