# Which WebGPU implementation a native Pulp build links.
#
# A native skia-builder slice ships its own static Dawn (libdawn_combined.a /
# dawn_combined.lib). When that archive is present it IS the WebGPU
# implementation: linking wgpu-native beside it duplicates every wgpu* symbol
# (a hard link error on Windows) and makes each plug-in bundle load its own
# libwgpu_native. FindSkia records the answer once, in the cache variable
# PULP_SKIA_PROVIDES_DAWN, so every consumer reads one decision in any
# directory scope. Consumers must not re-derive it from DAWN_LIBRARY: that is a
# normal variable a nested scope may not see, may hold the NOTFOUND sentinel,
# or may name an archive that no longer exists.

include_guard(GLOBAL)

# pulp_record_skia_dawn_provider(SKIA_FOUND <bool> [DAWN_ARCHIVE <path>])
#
# Sets PULP_SKIA_PROVIDES_DAWN (CACHE INTERNAL) to TRUE only when Skia was
# found and the Dawn archive of the selected slice exists on disk. A missing
# or empty value, the NOTFOUND sentinel, a relative path, and a stale path all
# record FALSE.
function(pulp_record_skia_dawn_provider)
    cmake_parse_arguments(PARSE_ARGV 0 _arg "" "SKIA_FOUND;DAWN_ARCHIVE" "")
    set(_provides FALSE)
    if(_arg_SKIA_FOUND
       AND IS_ABSOLUTE "${_arg_DAWN_ARCHIVE}"
       AND EXISTS "${_arg_DAWN_ARCHIVE}"
       AND NOT IS_DIRECTORY "${_arg_DAWN_ARCHIVE}")
        set(_provides TRUE)
    endif()
    set(PULP_SKIA_PROVIDES_DAWN ${_provides} CACHE INTERNAL
        "The selected Skia slice supplies the Dawn WebGPU implementation" FORCE)
endfunction()

# pulp_gpu_links_wgpu_native(<out> WEBGPU_TARGET <bool>)
#
# Sets <out> to TRUE when a GPU target must link the wgpu-native `webgpu`
# target: WebGPU is enabled, the target exists (pass `TARGET webgpu` as a
# bool), and Skia does not already supply Dawn.
function(pulp_gpu_links_wgpu_native out)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "" "WEBGPU_TARGET" "")
    set(_links FALSE)
    if(PULP_HAS_WEBGPU AND _arg_WEBGPU_TARGET AND NOT PULP_SKIA_PROVIDES_DAWN)
        set(_links TRUE)
    endif()
    set(${out} ${_links} PARENT_SCOPE)
endfunction()
