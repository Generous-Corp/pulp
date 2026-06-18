# PulpPortable.cmake — pulp_assert_portable_bundle(target)
#
# Guards against the "works on the build box, breaks when shared" footgun: a
# binary that bakes a build-tree absolute path and reads an asset from it at
# runtime (e.g. a TRIAZ_FRAME_SVG="${CMAKE_CURRENT_SOURCE_DIR}/..." compile
# definition fed to std::ifstream). On any machine but the build box that path
# is missing → the app silently degrades. This bit TRIAZ on 2026-06-18: a
# signed, notarized standalone fell back to the generic auto-Parameters panel on
# every Mac but the one it was built on, with nothing flagging it.
#
# This adds a POST_BUILD step that scans the target's binary for the consumer's
# source / build dir baked in as a string. Default is WARN (loud, but never
# blocks an inner-loop build — e.g. __FILE__ in a Debug assert could trip it);
# set -DPULP_STRICT_PORTABLE=ON (recommended for release / ship builds) to make a
# finding fail the build.
#
# Fix a finding by EMBEDDING the asset — pulp_embed_files() / pulp_add_binary_data()
# compile it into the binary — or bundle it into Resources and load via an
# executable-relative path. Never read an absolute build-tree path at runtime.
function(pulp_assert_portable_bundle target)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        return()  # no interpreter → skip silently; not worth blocking a build
    endif()
    set(_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/check_portable_binary.py")
    if(NOT EXISTS "${_script}")
        return()
    endif()
    set(_strict_arg "")
    if(PULP_STRICT_PORTABLE)
        set(_strict_arg "--strict")
    endif()
    # $<TARGET_FILE:target> resolves to the executable inside Contents/MacOS for
    # a MACOSX_BUNDLE, or the bare binary otherwise. CMAKE_SOURCE_DIR /
    # CMAKE_BINARY_DIR here are the CONSUMER project's dirs — exactly the
    # prefixes that should never appear baked into a shipped binary.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${_script}"
                "$<TARGET_FILE:${target}>"
                --forbid-prefix "${CMAKE_SOURCE_DIR}"
                --forbid-prefix "${CMAKE_BINARY_DIR}"
                --label "${target}"
                ${_strict_arg}
        VERBATIM
        COMMENT "Portability check: ${target} must not bake build-tree paths")
endfunction()
