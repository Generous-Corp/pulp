cmake_minimum_required(VERSION 3.24)

# SOURCE LINT — every macOS plug-in bundle helper must write Contents/PkgInfo.
#
# Deliberately labelled a lint, not a proof, exactly like the runtime-staging
# call-site check it sits beside: it greps helper text and cannot tell you the
# command ran or that a file landed.
#
# It exists because CLAP shipped without it. VST3, AU and AAX each wrote
# PkgInfo; the CLAP path did not, and nothing noticed, because the omission is
# invisible from inside the build — the bundle links, loads, validates and
# hosts identically. It shows up only in Finder, where a Pulp-built `.clap`
# reports kMDItemContentType `public.folder` while the `.vst3` from the same
# build reports `com.apple.generic-bundle`.
#
# `.vst3` and `.component` are extensions Launch Services already recognises,
# so they tolerate a missing declaration; `.clap` is not recognised at all, so
# the package declaration is the only thing distinguishing the bundle from an
# ordinary directory. A bundle that reads as a folder can be opened and taken
# apart in Finder, and can never render a bundle icon.

if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

# Each entry: <helper file>|<target expression whose bundle must get PkgInfo>
set(_expected_sites
    "tools/cmake/PulpPluginFormats.cmake|\${target}_VST3"
    "tools/cmake/PulpPluginFormats.cmake|\${target}_CLAP"
    "tools/cmake/PulpPluginFormats.cmake|\${target}_AU"
    "tools/cmake/PulpPluginFormats.cmake|\${target}_AAX"
)

set(_failures "")
foreach(_site IN LISTS _expected_sites)
    string(REPLACE "|" ";" _parts "${_site}")
    list(GET _parts 0 _rel_path)
    list(GET _parts 1 _target_expr)

    set(_abs "${PULP_SOURCE_DIR}/${_rel_path}")
    if(NOT EXISTS "${_abs}")
        list(APPEND _failures "${_rel_path}: file not found")
        continue()
    endif()
    file(READ "${_abs}" _content)

    # The generator expression naming this target's bundle, followed by the
    # PkgInfo path. Searched as a plain substring: the file is read as data,
    # so `${...}` is not expanded, but `$`, `{` and `}` are regex
    # metacharacters and string(FIND) sidesteps them.
    set(_needle
        "$<TARGET_BUNDLE_DIR:${_target_expr}>/Contents/PkgInfo")
    string(FIND "${_content}" "${_needle}" _found)
    if(_found EQUAL -1)
        list(APPEND _failures "${_rel_path}: nothing writes ${_needle}")
    endif()
endforeach()

if(_failures)
    string(REPLACE ";" "\n  " _rendered "${_failures}")
    message(FATAL_ERROR
        "macOS bundle PkgInfo call sites are incomplete:\n  ${_rendered}\n"
        "A plug-in bundle with no Contents/PkgInfo is not declared a package, "
        "so Finder treats it as a browsable folder rather than a single opaque "
        "artifact.")
endif()

message(STATUS "bundle_pkginfo_call_sites_verified=true")
