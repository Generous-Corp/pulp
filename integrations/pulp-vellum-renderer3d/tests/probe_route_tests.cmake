# The tools/scene3d route smokes, run against this package's probe. Each
# script drives the probe's command line for one renderer feature and verifies
# the fields it prints; the probe is the only thing that varies.
# Included from tests/CMakeLists.txt, which has found Python3.
set(_r3d_route_tools "${PULP_ROOT_DIR}/tools/scene3d")

# Smokes that print "<name>_verified=true". The route name is also the script
# stem: renderer_probe_<name>_smoke.py.
set(_r3d_routes
    non_base_texture_route derived_normal_route alpha_blend_sort_route
    alpha_mask_route vertex_color_route double_sided_route unlit_route
    emissive_route sampler_route mipmap_sampler_route
    base_color_transform_route resource_floor_route directional_light_route
    point_light_route spot_light_route transform_animation_route
    advanced_material_route unsupported_mesh_route perspective_camera_route
    orthographic_camera_route)

foreach(_route IN LISTS _r3d_routes)
    string(REPLACE "_" "-" _route_dashed "${_route}")
    set(_name pulp-vellum-renderer3d-probe-${_route_dashed})
    add_test(NAME ${_name}
        COMMAND ${Python3_EXECUTABLE}
            "${_r3d_route_tools}/renderer_probe_${_route}_smoke.py"
            --probe-tool $<TARGET_FILE:pulp-vellum-renderer3d-probe>)
    set_tests_properties(${_name} PROPERTIES
        LABELS pulp-vellum-renderer3d
        PASS_REGULAR_EXPRESSION "renderer_probe_${_route}_verified=true")
endforeach()

# The two smokes whose success line is not the shared "_verified=true" one.
add_test(NAME pulp-vellum-renderer3d-probe-material-floor
    COMMAND ${Python3_EXECUTABLE}
        "${_r3d_route_tools}/renderer_probe_material_floor_smoke.py"
        --probe-tool $<TARGET_FILE:pulp-vellum-renderer3d-probe>
        --fixture "${PULP_ROOT_DIR}/test/fixtures/scene3d/BoxTextured/BoxTextured.glb")
set_tests_properties(pulp-vellum-renderer3d-probe-material-floor PROPERTIES
    LABELS pulp-vellum-renderer3d
    PASS_REGULAR_EXPRESSION "renderer_probe_material_floor_verified=boxtextured")

add_test(NAME pulp-vellum-renderer3d-probe-adapter-selection
    COMMAND ${Python3_EXECUTABLE}
        "${_r3d_route_tools}/renderer_probe_adapter_selection_route_smoke.py"
        --probe-tool $<TARGET_FILE:pulp-vellum-renderer3d-probe>)
set_tests_properties(pulp-vellum-renderer3d-probe-adapter-selection PROPERTIES
    LABELS pulp-vellum-renderer3d
    PASS_REGULAR_EXPRESSION
        "renderer_probe_adapter_selection_route_case=null-backend.*renderer_probe_adapter_selection_route_case=forced-fallback.*renderer_probe_adapter_selection_route_verified=true")
