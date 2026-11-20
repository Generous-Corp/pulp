# Generate an RmlUi UI package once a native RmlUi editor host is supplied.
#
# VellumRmlUi currently exports the renderer-neutral Backend and model seam, but
# not an Editor that attaches that backend to a plug-in parent. Keep this
# wrapper explicit so a consumer cannot accidentally substitute the Vellum
# renderer (which brings Skia/Dawn) or Pulp's frozen view seam.
function(pulp_add_rmlui_editor_package package)
    cmake_parse_arguments(PARSE_ARGV 1 _rmlui_pkg ""
        "DOCUMENT;STYLESHEET;EDITOR_TARGET" "ASSET_DIRS")
    if(NOT _rmlui_pkg_DOCUMENT OR NOT _rmlui_pkg_STYLESHEET OR
       NOT _rmlui_pkg_EDITOR_TARGET)
        message(FATAL_ERROR "${package}: pulp_add_rmlui_editor_package needs "
            "DOCUMENT, STYLESHEET and EDITOR_TARGET")
    endif()
    if(NOT EXISTS "${_rmlui_pkg_DOCUMENT}")
        message(FATAL_ERROR "${package}: RmlUi document does not exist: ${_rmlui_pkg_DOCUMENT}")
    endif()
    if(NOT EXISTS "${_rmlui_pkg_STYLESHEET}")
        message(FATAL_ERROR "${package}: RmlUi stylesheet does not exist: ${_rmlui_pkg_STYLESHEET}")
    endif()
    if(NOT TARGET "${_rmlui_pkg_EDITOR_TARGET}")
        message(FATAL_ERROR "${package}: EDITOR_TARGET '${_rmlui_pkg_EDITOR_TARGET}' is not a target")
    endif()
    get_target_property(_function "${_rmlui_pkg_EDITOR_TARGET}"
        PULP_EDITOR_FACTORY_FUNCTION)
    get_target_property(_header "${_rmlui_pkg_EDITOR_TARGET}"
        PULP_EDITOR_FACTORY_HEADER)
    if(NOT _function OR NOT _header)
        message(FATAL_ERROR "${package}: EDITOR_TARGET must provide the editor-host "
            "factory properties; Vellum must export a native-parent RmlUi Editor first")
    endif()
    if(package MATCHES "::")
        string(REPLACE "::" "-" _package_target "${package}")
    else()
        set(_package_target "${package}")
    endif()
    if(TARGET "${package}" OR TARGET "${_package_target}")
        message(FATAL_ERROR "RmlUi editor package target '${package}' already exists")
    endif()
    add_library("${_package_target}" INTERFACE)
    if(package MATCHES "::")
        add_library("${package}" ALIAS "${_package_target}")
    endif()
    target_link_libraries("${_package_target}" INTERFACE "${_rmlui_pkg_EDITOR_TARGET}")
    set_target_properties("${_package_target}" PROPERTIES
        PULP_EDITOR_FACTORY_FUNCTION "${_function}"
        PULP_EDITOR_FACTORY_HEADER "${_header}"
        PULP_RMLUI_DOCUMENT "${_rmlui_pkg_DOCUMENT}"
        PULP_RMLUI_STYLESHEET "${_rmlui_pkg_STYLESHEET}"
        PULP_RMLUI_ASSET_DIRS "${_rmlui_pkg_ASSET_DIRS}")
endfunction()
