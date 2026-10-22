# pulp-screenshot - headless UI renderer (Apple build; backend selected at runtime)
if(APPLE)
    add_executable(pulp-screenshot pulp_screenshot.cpp)
    target_compile_definitions(pulp-screenshot PRIVATE
        PULP_SCREENSHOT_HAS_DESIGN_IMPORT=$<BOOL:${PULP_ENABLE_DESIGN_IMPORT}>)
    target_link_libraries(pulp-screenshot PRIVATE
        pulp::view
        pulp::state
    )
    if(PULP_ENABLE_DESIGN_IMPORT)
        # Keep the importer edge explicit: --design-ir is the only screenshot
        # mode that needs the authoring/codegen archive.
        target_link_libraries(pulp-screenshot PRIVATE pulp::design-import)
    endif()
endif()
