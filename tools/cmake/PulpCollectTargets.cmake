# Recursively collect build-system targets below a configured CMake directory.
#
# Usage:
#   pulp_collect_targets_recursive(result_var directory
#       TYPES EXECUTABLE STATIC_LIBRARY)
#
# With no TYPES list, every non-imported, non-alias target reported by
# BUILDSYSTEM_TARGETS is returned.
include_guard(GLOBAL)

function(pulp_collect_targets_recursive out_var directory)
    cmake_parse_arguments(PCT "" "" "TYPES" ${ARGN})

    get_property(local_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(child_directories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    set(collected_targets)

    foreach(local_target IN LISTS local_targets)
        if(TARGET "${local_target}")
            get_target_property(target_type "${local_target}" TYPE)
            if(NOT PCT_TYPES OR target_type IN_LIST PCT_TYPES)
                list(APPEND collected_targets "${local_target}")
            endif()
        endif()
    endforeach()

    foreach(child_directory IN LISTS child_directories)
        pulp_collect_targets_recursive(
            child_targets
            "${child_directory}"
            TYPES ${PCT_TYPES}
        )
        list(APPEND collected_targets ${child_targets})
    endforeach()

    list(REMOVE_DUPLICATES collected_targets)
    set(${out_var} "${collected_targets}" PARENT_SCOPE)
endfunction()
