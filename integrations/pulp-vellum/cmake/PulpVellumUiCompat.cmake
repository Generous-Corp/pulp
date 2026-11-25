# PulpVellumUiCompat.cmake: Vellum-backed compatibility targets for the Pulp
# UI names an installed SDK consumer links.
#
# Downstream plug-ins link Pulp::view, Pulp::canvas, Pulp::render,
# Pulp::view-native and Pulp::design-import. Vellum owns the implementations
# behind those names and installs them as its mirror targets. In an SDK built
# without Pulp's own UI libraries (PULP_SDK_UI_FROM_VELLUM, which PulpConfig
# checks before including this file), pulp_define_vellum_ui_compat_targets()
# defines each name as an INTERFACE IMPORTED target that links the matching
# Vellum targets, plus the lower-case pulp:: alias the in-tree build provides.
#
# Two guards keep it from ever standing in for part of Pulp's own UI: nothing
# is defined while Pulp::view-core is exported, and a name Pulp still exports
# is left alone.
#
# The caller loads the Vellum package first. A name whose Vellum backing target
# is missing stays undefined, so a consumer fails at generate time on that name
# instead of linking an empty interface.
#
# PULP_SDK_VELLUM_UI_COMPAT_TARGETS lists, in the caller's scope, the names that
# are Vellum-backed: every Pulp:: UI target whose PULP_UI_PROVIDER is "vellum",
# so a second find_package(Pulp) reports the same list as the first.
#
# One GPU device layer per binary. Vellum's provider library, which every
# Vellum-backed name links, carries its own gpu_runtime and Dawn and exports
# both the gpu_runtime:: API and the wgpu C API. Pulp::gpu-device and
# Pulp::gpu-audio (and Pulp::host through Pulp::gpu-audio) link Pulp's static
# gpu_runtime instead. A binary that links both holds two device layers with
# separate device, bootstrap and logging state. The provider keeps its Dawn
# bootstrap registry and Skia log bridge internal, so the static side cannot be
# pointed at it; configure refuses such a binary instead. The check runs when
# the directory that called find_package(Pulp) finishes, over every
# executable, shared library and module defined in it or below it.

include_guard(GLOBAL)

set(_PULP_VELLUM_GPU_OWNER_STATIC
    "Pulp's static gpu_runtime (VellumGpuRuntime::Device and its Dawn)")
set(_PULP_VELLUM_GPU_OWNER_PROVIDER
    "the gpu_runtime and Dawn inside Vellum's provider library")

# Splits one link item, generator expressions included, into the names it may
# refer to. Conditions are not evaluated, so a conditional dependency counts.
function(_pulp_vellum_link_tokens item out)
    string(REGEX REPLACE "\\$<[A-Za-z_]+:" ";" _text "${item}")
    string(REGEX REPLACE "[$<>,]" ";" _text "${_text}")
    set(_tokens "")
    foreach(_token IN LISTS _text)
        string(REGEX REPLACE "^:+" "" _token "${_token}")
        if(NOT _token STREQUAL "")
            list(APPEND _tokens "${_token}")
        endif()
    endforeach()
    set(${out} "${_tokens}" PARENT_SCOPE)
endfunction()

function(_pulp_vellum_mark_gpu_runtime_owners)
    foreach(_target IN ITEMS Pulp::gpu-device Pulp::gpu-audio VellumGpuRuntime::Device
            VellumGpuRuntime::Bootstrap VellumGpuRuntime::SkiaLogBridge)
        if(TARGET "${_target}")
            set_property(TARGET "${_target}" PROPERTY PULP_GPU_RUNTIME_OWNER static)
        endif()
    endforeach()
    foreach(_name IN LISTS ARGN)
        set_property(TARGET "Pulp::${_name}" PROPERTY PULP_GPU_RUNTIME_OWNER provider)
        get_target_property(_backing "Pulp::${_name}" INTERFACE_LINK_LIBRARIES)
        foreach(_target IN LISTS _backing)
            if(_target MATCHES "^Vellum::" AND TARGET "${_target}")
                set_property(TARGET "${_target}" PROPERTY PULP_GPU_RUNTIME_OWNER provider)
            endif()
        endforeach()
    endforeach()
endfunction()

function(_pulp_vellum_directories dir out)
    set(_all "${dir}")
    get_property(_children DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
        _pulp_vellum_directories("${_child}" _below)
        list(APPEND _all ${_below})
    endforeach()
    set(${out} "${_all}" PARENT_SCOPE)
endfunction()

# Sets `out_static` and `out_provider` to the first target in the link closure
# of `item` that owns each device layer, or to "" when none does.
function(_pulp_vellum_gpu_owners_of item out_static out_provider)
    _pulp_vellum_link_tokens("${item}" _queue)
    set(_seen "")
    set(_static "")
    set(_provider "")
    while(_queue)
        list(POP_FRONT _queue _dep)
        if(NOT TARGET "${_dep}" OR "${_dep}" IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_dep}")
        get_target_property(_real "${_dep}" ALIASED_TARGET)
        if(NOT _real)
            set(_real "${_dep}")
        endif()
        get_target_property(_owner "${_real}" PULP_GPU_RUNTIME_OWNER)
        if(_owner STREQUAL "static" AND NOT _static)
            set(_static "${_dep}")
        elseif(_owner STREQUAL "provider" AND NOT _provider)
            set(_provider "${_dep}")
        endif()
        get_target_property(_next "${_real}" INTERFACE_LINK_LIBRARIES)
        foreach(_next_item IN LISTS _next)
            _pulp_vellum_link_tokens("${_next_item}" _next_tokens)
            list(APPEND _queue ${_next_tokens})
        endforeach()
    endwhile()
    set(${out_static} "${_static}" PARENT_SCOPE)
    set(${out_provider} "${_provider}" PARENT_SCOPE)
endfunction()

# "X" when the binary links the owner itself, "X (through Y)" otherwise.
function(_pulp_vellum_gpu_route root owner out)
    if(root STREQUAL owner)
        set(${out} "${owner}" PARENT_SCOPE)
    else()
        set(${out} "${owner} (through ${root})" PARENT_SCOPE)
    endif()
endfunction()

function(_pulp_vellum_check_gpu_runtime_owners dir)
    _pulp_vellum_directories("${dir}" _dirs)
    foreach(_dir IN LISTS _dirs)
        get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            get_target_property(_type "${_target}" TYPE)
            if(NOT _type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
                continue()
            endif()
            get_target_property(_links "${_target}" LINK_LIBRARIES)
            if(NOT _links)
                continue()
            endif()
            set(_static_route "")
            set(_provider_route "")
            foreach(_item IN LISTS _links)
                _pulp_vellum_link_tokens("${_item}" _roots)
                foreach(_root IN LISTS _roots)
                    _pulp_vellum_gpu_owners_of("${_root}" _static _provider)
                    if(_static AND NOT _static_route)
                        _pulp_vellum_gpu_route("${_root}" "${_static}" _static_route)
                    endif()
                    if(_provider AND NOT _provider_route)
                        _pulp_vellum_gpu_route("${_root}" "${_provider}" _provider_route)
                    endif()
                endforeach()
            endforeach()
            if(_static_route AND _provider_route)
                message(FATAL_ERROR
                    "Pulp: ${_target} links two GPU device layers: "
                    "${_PULP_VELLUM_GPU_OWNER_STATIC} from ${_static_route}, and "
                    "${_PULP_VELLUM_GPU_OWNER_PROVIDER} from ${_provider_route}. "
                    "A binary may hold only one. In an SDK that serves its UI "
                    "targets from Vellum, link either those UI targets or "
                    "Pulp::gpu-device, Pulp::gpu-audio and Pulp::host, not both.")
            endif()
        endforeach()
    endforeach()
endfunction()

function(_pulp_vellum_ui_compat_define name)
    cmake_parse_arguments(PARSE_ARGV 1 _compat "" "" "REQUIRES;OPTIONAL")
    set(_target "Pulp::${name}")
    if(TARGET "${_target}")
        return()
    endif()
    foreach(_dependency IN LISTS _compat_REQUIRES)
        if(NOT TARGET "${_dependency}")
            return()
        endif()
    endforeach()
    set(_links ${_compat_REQUIRES})
    foreach(_dependency IN LISTS _compat_OPTIONAL)
        if(TARGET "${_dependency}")
            list(APPEND _links "${_dependency}")
        endif()
    endforeach()
    add_library("${_target}" INTERFACE IMPORTED)
    set_target_properties("${_target}" PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_links}"
        INTERFACE_COMPILE_FEATURES cxx_std_20
        PULP_UI_PROVIDER vellum)
    if(NOT TARGET "pulp::${name}")
        add_library("pulp::${name}" ALIAS "${_target}")
    endif()
endfunction()

# Pulp::vellum is the Pulp-owned editor bridge (format-core plus the Vellum
# parameter ports). It rides along with the view names when the SDK exports
# it, because those are the names a plug-in editor links.
function(pulp_define_vellum_ui_compat_targets)
    if(NOT TARGET Pulp::view-core)
        _pulp_vellum_ui_compat_define(canvas
            REQUIRES Vellum::MirrorCanvas)
        _pulp_vellum_ui_compat_define(render
            REQUIRES Vellum::MirrorRender_2d)
        _pulp_vellum_ui_compat_define(design-import
            REQUIRES Vellum::MirrorDesign_import)
        _pulp_vellum_ui_compat_define(view-native
            REQUIRES Vellum::MirrorView_core
            OPTIONAL Pulp::vellum)
        # The umbrella keeps the importer and the scripted view, as Pulp::view
        # does in an SDK built with them.
        _pulp_vellum_ui_compat_define(view
            REQUIRES Vellum::MirrorView_core Vellum::MirrorDesign_import
            OPTIONAL Vellum::MirrorView_script Pulp::vellum)
    endif()
    set(_defined "")
    foreach(_name IN ITEMS canvas render design-import view-native view)
        if(TARGET "Pulp::${_name}")
            get_target_property(_provider "Pulp::${_name}" PULP_UI_PROVIDER)
            if(_provider STREQUAL "vellum")
                list(APPEND _defined "${_name}")
            endif()
        endif()
    endforeach()
    if(_defined)
        _pulp_vellum_mark_gpu_runtime_owners(${_defined})
        get_property(_armed DIRECTORY PROPERTY _PULP_VELLUM_GPU_OWNER_CHECK)
        if(NOT _armed)
            set_property(DIRECTORY PROPERTY _PULP_VELLUM_GPU_OWNER_CHECK ON)
            cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                CALL _pulp_vellum_check_gpu_runtime_owners "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
    endif()
    set(PULP_SDK_VELLUM_UI_COMPAT_TARGETS "${_defined}" PARENT_SCOPE)
endfunction()
