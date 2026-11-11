# rmlui-native: RmlUi on a pulp-native Metal surface. It brings no renderer
# and no RmlUi build of its own: RmlUi comes from Vellum's installed
# VellumRmlUi::Native, and the caller supplies an Rml::RenderInterface.

find_package(VellumRmlUi CONFIG REQUIRED COMPONENTS Native OPTIONAL_COMPONENTS Metal)

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
