# pulp-vellum-components: the retained Pulp view components (panic button,
# lasso, preferences panel) as Vellum views, plus a catalogue
# that builds each by stable id. Generic widgets live on the Vellum view layer;
# Pulp-owned services (processor panic, properties file, model store) arrive
# through the catalogue's context. Core never links it.
find_package(Vellum CONFIG REQUIRED)

add_library(pulp-vellum-components STATIC
    src/atlas_inventory.cpp
    src/catalogue.cpp
    src/field_edit.cpp
    src/inspector_overlay.cpp
    src/inspector_overlay_atlas_panel.cpp
    src/inspector_overlay_field_edit.cpp
    src/inspector_overlay_knob_panel.cpp
    src/inspector_overlay_pass_panel.cpp
    src/inspector_overlay_text_edit.cpp
    src/inspector_overlay_zoom.cpp
    src/inspector_window.cpp
    src/knob_panel.cpp
    src/overlay_geometry.cpp
    src/render_pass_stats.cpp
    src/text_edit.cpp)
add_library(pulp::vellum-components ALIAS pulp-vellum-components)
target_include_directories(pulp-vellum-components PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-vellum-components PUBLIC cxx_std_20)
# pulp::state and pulp::format-core are PUBLIC because the installed headers
# (preferences_panel.hpp, panic_target.hpp) include their headers.
target_link_libraries(pulp-vellum-components PUBLIC
    pulp::state pulp::format-core Vellum::MirrorView_core)
