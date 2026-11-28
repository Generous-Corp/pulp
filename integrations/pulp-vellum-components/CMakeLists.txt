# pulp-vellum-components: the retained Pulp view components (panic button,
# lasso, preferences panel, timeline arranger) as Vellum views, plus a catalogue
# that builds each by stable id. Generic widgets live on the Vellum view layer;
# Pulp-owned services (processor panic, properties file, model store) arrive
# through the catalogue's context. Core never links it.
find_package(Vellum CONFIG REQUIRED)

add_library(pulp-vellum-components STATIC
    src/arranger_view.cpp
    src/catalogue.cpp
    src/gpu_health_content.cpp
    src/inspector_window.cpp
    src/overlay_geometry.cpp
    src/view_inspector.cpp)
add_library(pulp::vellum-components ALIAS pulp-vellum-components)
target_include_directories(pulp-vellum-components PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-vellum-components PUBLIC cxx_std_20)
# pulp::state and pulp::format-core are PUBLIC because the installed headers
# (preferences_panel.hpp, panic_target.hpp) include their headers.
# pulp::timeline, pulp::timeline-editor and pulp::timebase are PUBLIC for the same
# reason: the timeline views name the document model and the edit-intent kernel.
target_link_libraries(pulp-vellum-components PUBLIC
    pulp::state pulp::format-core pulp::timeline pulp::timeline-editor pulp::timebase
    Vellum::MirrorView_core)
