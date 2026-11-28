# Apple ld scans a static archive once. The CLI intentionally exposes the
# complete design/import and inspector command surface, so a single archive
# can contain both the public entry point and its implementation-only helper
# TUs (for example WindowHost/design/inspector RPC). Those helper objects can
# become newly-needed while the archive is being scanned and are then skipped,
# leaving the otherwise valid CLI with undefined symbols. Force-load only the
# CLI's implementation archives; this keeps plugin consumers unchanged while
# making the installed SDK CLI link deterministic on arm64 macOS.
#
# Four of those archives are Pulp's own view, canvas and design-import
# libraries. PULP_UI_FROM_VELLUM_IN_TREE leaves them out, and skipping them
# here would hand back exactly the undefined-symbol CLI this exists to prevent,
# so that mode refuses the CLI until its link is ported to the Vellum-backed
# names.
function(pulp_cli_force_load_archives target)
    if(NOT APPLE)
        return()
    endif()
    set(_ui_archives pulp-view-core pulp-design-import pulp-view-script pulp-canvas)
    if(PULP_UI_FROM_VELLUM_IN_TREE)
        message(FATAL_ERROR
            "${target} force-loads Pulp's own UI archives (${_ui_archives}), which "
            "PULP_UI_FROM_VELLUM_IN_TREE leaves out; without them the CLI links with "
            "undefined symbols. Port its link to the Vellum-backed names first.")
    endif()
    foreach(_archive IN LISTS _ui_archives ITEMS pulp-runtime pulp-inspect-protocol)
        if(TARGET ${_archive})
            target_link_options(${target} PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:${_archive}>")
        endif()
    endforeach()
endfunction()
