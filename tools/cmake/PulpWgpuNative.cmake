# PulpWgpuNative.cmake — digest-pinned wgpu-native runtime archives.
#
# wgpu-native (gfx-rs, MIT OR Apache-2.0) publishes prebuilt per-platform zips.
# WebGPU-distribution's FetchWgpuNativePrecompiled.cmake would download the one
# matching the target with a bare `FetchContent_Declare(URL ...)` and no digest.
# Pulp never lets it: the configure step resolves the archive here, checks its
# SHA-256 against tools/deps/manifest.json (the `artifacts` list of the
# `wgpu-native` entry, the single record of every pinned asset digest), and
# hands the verified tree to FetchContent through FETCHCONTENT_SOURCE_DIR_<NAME>
# so the upstream download never runs.
#
# setup.sh primes the shared cache after checking the same archive digest. The
# cache holds only extracted files, so configure re-checks it against the
# pinned digest of the extracted tree (`tree_sha256`) and never trusts a cache
# directory merely because it exists. A cache that does not match is ignored
# and a verified build-local copy is downloaded instead (or, when
# FETCHCONTENT_FULLY_DISCONNECTED is ON, configure stops and says how to fix
# it).

include_guard(GLOBAL)

# Read the pinned URL and SHA-256 of one release asset.
#   asset   — the asset stem, e.g. wgpu-macos-aarch64-release
#   version — the wgpu-native release CMake pinned; must equal the manifest's
# <out_tree_sha> receives the digest of the extracted tree (see
# pulp_wgpu_native_tree_sha256). PULP_WGPU_NATIVE_MANIFEST overrides the
# manifest path (tests use it).
function(pulp_wgpu_native_pin asset version out_url out_sha out_tree_sha)
    if(PULP_WGPU_NATIVE_MANIFEST)
        set(_manifest "${PULP_WGPU_NATIVE_MANIFEST}")
    else()
        set(_manifest "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../deps/manifest.json")
    endif()
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR
            "wgpu-native: the dependency manifest holding its pinned digests is "
            "missing (${_manifest}). Restore tools/deps/manifest.json from git.")
    endif()
    file(READ "${_manifest}" _json)
    string(JSON _count LENGTH "${_json}" dependencies)
    math(EXPR _last "${_count} - 1")
    set(_entry "")
    foreach(_i RANGE ${_last})
        string(JSON _name GET "${_json}" dependencies ${_i} name)
        if(_name STREQUAL "wgpu-native")
            string(JSON _entry GET "${_json}" dependencies ${_i})
            break()
        endif()
    endforeach()
    if(_entry STREQUAL "")
        message(FATAL_ERROR
            "wgpu-native: no `wgpu-native` entry in ${_manifest}. Its release "
            "asset digests live there; restore the entry before configuring.")
    endif()
    string(JSON _manifest_version GET "${_entry}" version)
    if(NOT _manifest_version STREQUAL version)
        message(FATAL_ERROR
            "wgpu-native: CMake pins ${version} but ${_manifest} records "
            "${_manifest_version}. Bump both together, re-recording every asset "
            "sha256 in the manifest's wgpu-native `artifacts` list.")
    endif()
    string(JSON _n LENGTH "${_entry}" artifacts)
    math(EXPR _last "${_n} - 1")
    foreach(_i RANGE ${_last})
        string(JSON _asset GET "${_entry}" artifacts ${_i} name)
        if(_asset STREQUAL asset)
            string(JSON _url GET "${_entry}" artifacts ${_i} url)
            string(JSON _sha GET "${_entry}" artifacts ${_i} sha256)
            string(JSON _tree GET "${_entry}" artifacts ${_i} tree_sha256)
            string(TOLOWER "${_sha}" _sha)
            string(TOLOWER "${_tree}" _tree)
            set(${out_url} "${_url}" PARENT_SCOPE)
            set(${out_sha} "${_sha}" PARENT_SCOPE)
            set(${out_tree_sha} "${_tree}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR
        "wgpu-native: no pinned digest for release asset '${asset}' (${version}) "
        "in ${_manifest}. Download ${asset}.zip from the ${version} release into "
        "an empty directory, record its `shasum -a 256` (sha256) and the "
        "digest of its extracted tree (tree_sha256, see "
        "tools/deps/wgpu_native_digests.py) in the wgpu-native `artifacts` "
        "list, then reconfigure.")
endfunction()

# Digest of an extracted archive tree: SHA-256 over one "<relative path>
# <sha256>\n" line per regular file, sorted by path. It lets configure verify a
# shared-cache directory that holds only the extracted files, whoever primed it.
# tools/deps/wgpu_native_digests.py computes the same value.
function(pulp_wgpu_native_tree_sha256 dir out_sha)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    list(SORT _files)
    set(_lines "")
    foreach(_f IN LISTS _files)
        file(SHA256 "${dir}/${_f}" _h)
        string(APPEND _lines "${_f} ${_h}\n")
    endforeach()
    string(SHA256 _digest "${_lines}")
    set(${out_sha} "${_digest}" PARENT_SCOPE)
endfunction()

# Download one asset, refuse it unless its SHA-256 matches the pin, and extract
# it beneath dest_root. A previous verified extraction is reused.
function(pulp_wgpu_native_fetch_verified asset version dest_root out_dir)
    pulp_wgpu_native_pin("${asset}" "${version}" _url _sha _tree_sha)
    set(_dir "${dest_root}/${asset}-${version}")
    set(_stamp "${_dir}.sha256")
    if(EXISTS "${_dir}" AND EXISTS "${_stamp}")
        file(READ "${_stamp}" _recorded)
        string(STRIP "${_recorded}" _recorded)
        if(_recorded STREQUAL "${_sha} ${_tree_sha}")
            # The stamp says it was verified once; the files must still match.
            pulp_wgpu_native_tree_sha256("${_dir}" _reused_tree)
            if(_reused_tree STREQUAL _tree_sha)
                set(${out_dir} "${_dir}" PARENT_SCOPE)
                return()
            endif()
            message(STATUS
                "wgpu-native: build-local copy ${_dir} changed since it was "
                "verified (found tree ${_reused_tree}); fetching it again.")
        endif()
    endif()
    if(FETCHCONTENT_FULLY_DISCONNECTED)
        message(FATAL_ERROR
            "wgpu-native: FETCHCONTENT_FULLY_DISCONNECTED is ON, but no "
            "digest-verified copy of ${asset} ${version} is available (looked "
            "for ${_dir} and the shared source cache). Run "
            "`./setup.sh --deps-only` while online to prime the verified shared "
            "cache, or reconfigure once with FETCHCONTENT_FULLY_DISCONNECTED=OFF.")
    endif()

    file(MAKE_DIRECTORY "${dest_root}")
    set(_zip "${dest_root}/${asset}-${version}.zip")
    file(REMOVE "${_zip}")
    message(STATUS "wgpu-native: fetching ${_url}")
    file(DOWNLOAD "${_url}" "${_zip}" STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _rc)
    if(NOT _rc EQUAL 0)
        list(GET _status 1 _msg)
        file(REMOVE "${_zip}")
        message(FATAL_ERROR
            "wgpu-native: failed to download ${_url}: ${_msg}. Check the "
            "network, or prime the shared cache with `./setup.sh --deps-only`.")
    endif()
    file(SHA256 "${_zip}" _found)
    if(NOT _found STREQUAL _sha)
        file(REMOVE "${_zip}")
        message(FATAL_ERROR
            "wgpu-native: refusing ${asset}.zip from ${_url}: its SHA-256 does "
            "not match the pin.\n"
            "  expected: ${_sha}\n"
            "  found:    ${_found}\n"
            "The download was deleted and nothing was extracted. If the release "
            "asset was legitimately republished, verify it independently and "
            "update the sha256 for ${asset} in tools/deps/manifest.json "
            "(wgpu-native `artifacts`); otherwise treat the mirror as "
            "compromised and do not build against it.")
    endif()

    file(REMOVE_RECURSE "${_dir}.partial" "${_dir}")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_dir}.partial")
    pulp_wgpu_native_tree_sha256("${_dir}.partial" _found_tree)
    if(NOT _found_tree STREQUAL _tree_sha)
        file(REMOVE_RECURSE "${_dir}.partial")
        message(FATAL_ERROR
            "wgpu-native: ${asset}.zip matched its archive digest but extracted "
            "to an unexpected tree (expected ${_tree_sha}, found ${_found_tree}). "
            "The tree_sha256 for ${asset} in tools/deps/manifest.json is wrong; "
            "recompute it with tools/deps/wgpu_native_digests.py.")
    endif()
    file(RENAME "${_dir}.partial" "${_dir}")
    file(WRITE "${_stamp}" "${_sha} ${_tree_sha}\n")
    file(REMOVE "${_zip}")
    set(${out_dir} "${_dir}" PARENT_SCOPE)
endfunction()

# Choose the wgpu-native tree FetchContent will use for `asset`:
#   - an explicit developer override (FETCHCONTENT_SOURCE_DIR_<NAME> set by
#     hand or PULP_FETCHCONTENT_SOURCE_DIR_<NAME>) is honoured as-is, the same
#     contract every other FetchContent dependency has;
#   - the shared cache when its extracted files match the pinned tree digest;
#   - otherwise a verified build-local download.
# Sets <out_dir> to the chosen tree.
function(pulp_wgpu_native_select_source asset version out_dir)
    string(TOUPPER "${asset}" _upper)
    set(_var "FETCHCONTENT_SOURCE_DIR_${_upper}")
    pulp_wgpu_native_pin("${asset}" "${version}" _url _sha _tree_sha)
    set(_is_shared "${PULP_FETCHCONTENT_SOURCE_IS_SHARED_${_upper}}")
    if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
        if(NOT _is_shared)
            message(STATUS
                "wgpu-native: using explicit source override ${${_var}} "
                "(developer-supplied; not digest-verified)")
            set(${out_dir} "${${_var}}" PARENT_SCOPE)
            return()
        endif()
        if(EXISTS "${${_var}}")
            pulp_wgpu_native_tree_sha256("${${_var}}" _found_tree)
        else()
            set(_found_tree "<missing>")
        endif()
        if(_found_tree STREQUAL _tree_sha)
            set(${out_dir} "${${_var}}" PARENT_SCOPE)
            return()
        endif()
        message(STATUS
            "wgpu-native: shared cache ${${_var}} does not match the pinned tree "
            "digest (expected ${_tree_sha}, found ${_found_tree}); ignoring it "
            "and using a verified build-local copy. `./setup.sh --deps-only` "
            "re-primes the shared cache.")
    endif()
    pulp_wgpu_native_fetch_verified("${asset}" "${version}"
        "${CMAKE_BINARY_DIR}/_deps/pulp-wgpu-native" _dir)
    set(${out_dir} "${_dir}" PARENT_SCOPE)
endfunction()

# Call after WebGPU-distribution is made available. The archive it populated
# must be the verified tree pulp_register_wgpu_native_precompiled_source chose:
# if its asset selection (OS, ARCH, compiler) ever diverges from Pulp's, it
# names a different FetchContent entry, fetched an unpinned archive, and
# configure stops here.
function(pulp_wgpu_native_assert_used_verified_source)
    if(NOT PULP_WGPU_NATIVE_ASSET)
        return()  # no wgpu-native prebuilt on this platform
    endif()
    string(TOLOWER "${PULP_WGPU_NATIVE_ASSET}" _name)
    FetchContent_GetProperties(${_name} SOURCE_DIR _source POPULATED _populated)
    set(_ok FALSE)
    if(_populated AND EXISTS "${_source}" AND EXISTS "${PULP_WGPU_NATIVE_SOURCE_DIR}")
        file(REAL_PATH "${_source}" _source_real)
        file(REAL_PATH "${PULP_WGPU_NATIVE_SOURCE_DIR}" _verified_real)
        if(_source_real STREQUAL _verified_real)
            set(_ok TRUE)
        endif()
    endif()
    if(NOT _ok)
        message(FATAL_ERROR
            "wgpu-native: WebGPU-distribution did not use the digest-verified "
            "runtime archive ${PULP_WGPU_NATIVE_ASSET} "
            "(${PULP_WGPU_NATIVE_SOURCE_DIR}); populated='${_populated}' "
            "source='${_source}'. Its asset selection (OS, ARCH, compiler) no "
            "longer matches pulp_register_wgpu_native_precompiled_source in "
            "tools/cmake/PulpFetchContent.cmake; align the two so the pinned "
            "asset is the one fetched.")
    endif()
endfunction()
