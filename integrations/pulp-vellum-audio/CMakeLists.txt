# Pulp's audio display provider for Vellum meters and scopes. The provider is
# optional and depends only on Pulp runtime plus Vellum's value interfaces.
find_package(Vellum CONFIG REQUIRED)

add_library(pulp-vellum-audio STATIC src/audio_display_provider.cpp src/audio_widgets.cpp)
add_library(pulp::vellum-audio ALIAS pulp-vellum-audio)
target_include_directories(pulp-vellum-audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-vellum-audio PUBLIC cxx_std_20)
target_link_libraries(pulp-vellum-audio PUBLIC pulp::runtime pulp::dataplane-audio pulp::signal Vellum::Value Vellum::MirrorView_core)
