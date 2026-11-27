# A named aggregate lets CI compile every example-owned artifact without also
# building the unrelated core-test tree or custom install/utility targets.
include("${PULP_ROOT_DIR}/tools/cmake/PulpCollectTargets.cmake")
pulp_collect_targets_recursive(
    pulp_example_targets
    "${CMAKE_CURRENT_SOURCE_DIR}"
    TYPES EXECUTABLE STATIC_LIBRARY SHARED_LIBRARY MODULE_LIBRARY OBJECT_LIBRARY
)
if(APPLE)
    # AUv3 container assembly is a custom target rather than a library/executable.
    # Preserve the complete example bundle while excluding unrelated utility
    # targets that the root default build used to pull into this CI lane.
    pulp_collect_targets_recursive(
        pulp_example_utility_targets
        "${CMAKE_CURRENT_SOURCE_DIR}"
        TYPES UTILITY
    )
    list(FILTER pulp_example_utility_targets INCLUDE REGEX "_AUv3Host_Embed$")
    list(APPEND pulp_example_targets ${pulp_example_utility_targets})
endif()
add_custom_target(pulp-examples-all)
if(pulp_example_targets)
    add_dependencies(pulp-examples-all ${pulp_example_targets})
endif()
