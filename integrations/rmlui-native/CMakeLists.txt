# rmlui-native: RmlUi on a pulp-native Metal surface. It brings no renderer
# and no RmlUi build of its own: RmlUi comes from Vellum's installed
# VellumRmlUi::Native, and the caller supplies an Rml::RenderInterface.

find_package(VellumRmlUi CONFIG REQUIRED COMPONENTS Native)

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
