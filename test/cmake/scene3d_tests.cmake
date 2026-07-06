# Scene3D import, sidecar, preflight, and native-boundary test registrations.
# Included by test/CMakeLists.txt; add focused registrations to the included manifests.

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/scene3d_core_tests.cmake")

if(PULP_ENABLE_SCENE3D)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/scene3d_inspect_tests.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/scene3d_sidecar_tests.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/scene3d_preflight_tests.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/scene3d_boundary_golden_tests.cmake")
endif()
