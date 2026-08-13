include_guard(GLOBAL)

function(pulp_git_dirty source_dir output_variable)
    set(_pulp_git_dirty false)
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${source_dir}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}"
                status --porcelain --untracked-files=all
                --ignore-submodules=none
            RESULT_VARIABLE _pulp_git_status_result
            OUTPUT_VARIABLE _pulp_git_status
            ERROR_QUIET)
        if(NOT _pulp_git_status_result EQUAL 0 OR
           NOT "${_pulp_git_status}" STREQUAL "")
            set(_pulp_git_dirty true)
        endif()
    endif()
    set(${output_variable} "${_pulp_git_dirty}" PARENT_SCOPE)
endfunction()
