# Forge Modular — the fourth product on the shared chrome.
#
# Standalone only here. The AU/VST3/CLAP builds and the .vcvplugin live in
# pulp-modular-rack, which is also where the generator and the Rack SDK stay:
# the Rack SDK is GPLv3 and has no business in this tree.
pulp_add_plugin(ForgeModular
    FORMATS           Standalone
    SOURCES           ${FORGE_SOURCES}
    PLUGIN_NAME       "Forge Modular"
    BUNDLE_ID         "${FORGE_IDENTITY_BUNDLE_PREFIX}.modular"
    MANUFACTURER      "${FORGE_IDENTITY_MANUFACTURER}"
    VERSION           "${PROJECT_VERSION}"
    CATEGORY          Effect
    PLUGIN_CODE       "FrgM"
    MANUFACTURER_CODE "${FORGE_IDENTITY_AU_MANUFACTURER_CODE}"
)

# The same wiring the other products' cores get. Forge Modular runs the same
# chrome, so it needs the same view/canvas/render stack and the same generated
# includes -- diverging here is how it would stop being the same app.
if(TARGET ForgeModular_Core)
    target_include_directories(ForgeModular_Core PUBLIC
        ${FORGE_GENERATED_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(ForgeModular_Core PUBLIC
        forge_recipe_bank_data
        forge_drum_recipe_bank_data
        forge_font_data
        forge_model_catalog_data
        forge_type_icons
        pulp::host pulp::signal
        pulp::view pulp::view-script pulp::canvas pulp::render
        PRIVATE forge_param_map forge_scratch_workspace)
    if(FORGE_HAS_PULP_DESIGN_IMPORT)
        target_link_libraries(ForgeModular_Core PUBLIC forge_designer)
    endif()
    target_compile_definitions(ForgeModular_Core PUBLIC
        FORGE_UI_DIR="${FORGE_GENERATED_UI_DIR}"
        FORGE_HAS_PULP_DESIGN_IMPORT=$<BOOL:${FORGE_HAS_PULP_DESIGN_IMPORT}>)
endif()
