# pulp-vellum-scene: binds Vellum's installed Draco decoder to the glTF scene
# loader's decode hook, so a Pulp scene load needs no pulp::render. The scene
# model and loader (pulp::scene) stay in Pulp core. Core never links this
# package.
find_package(Vellum CONFIG REQUIRED)

if(NOT TARGET pulp::scene)
    message(FATAL_ERROR
        "PULP_ENABLE_PULP_VELLUM_SCENE=ON needs pulp::scene; configure with PULP_ENABLE_SCENE3D=ON")
endif()

add_library(pulp-vellum-scene STATIC src/draco_scene_adapter.cpp)
add_library(pulp::vellum-scene ALIAS pulp-vellum-scene)
target_include_directories(pulp-vellum-scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-vellum-scene PUBLIC cxx_std_20)
# Vellum::MirrorRender_2d carries <vellum/render/draco_decoder.hpp> and the
# decoder symbols (through Vellum::SkiaDawnProvider).
target_link_libraries(pulp-vellum-scene PUBLIC pulp::scene Vellum::MirrorRender_2d)
