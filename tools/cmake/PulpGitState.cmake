include_guard(GLOBAL)

function(pulp_git_tracked_dirty source_dir output_variable)
    set(_pulp_git_tracked_dirty false)
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${source_dir}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}"
                status --porcelain --untracked-files=no
                --ignore-submodules=untracked
            RESULT_VARIABLE _pulp_git_status_result
            OUTPUT_VARIABLE _pulp_git_status
            ERROR_QUIET)
        if(_pulp_git_status_result EQUAL 0 AND
           NOT "${_pulp_git_status}" STREQUAL "")
            set(_pulp_git_tracked_dirty true)
        endif()
    endif()
    set(${output_variable} "${_pulp_git_tracked_dirty}" PARENT_SCOPE)
endfunction()
