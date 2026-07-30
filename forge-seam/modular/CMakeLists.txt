# Forge Modular — the fourth product on the shared chrome.
#
# Every format, same as its three siblings. This was Standalone-only while the
# generator was assumed to be in-process -- that would have dragged the GPLv3
# Rack SDK into a plugin binary, which must never happen. It does not: the
# shell hands a prompt to a child process, so nothing here links the Rack SDK
# and the .vcvplugin stays in pulp-modular-rack where it belongs.
pulp_add_plugin(ForgeModular
    FORMATS           AU VST3 CLAP Standalone
    SOURCES           ${FORGE_SOURCES}
    PLUGIN_NAME       "Forge Modular"
    BUNDLE_ID         "${FORGE_IDENTITY_BUNDLE_PREFIX}.modular"
    MANUFACTURER      "${FORGE_IDENTITY_MANUFACTURER}"
    VERSION           "${PROJECT_VERSION}"
    CATEGORY          Effect
    # 'FrgR' for Rack, NOT 'FrgM': Forge MIDI already owns FrgM. The AU type
    # differs (aufx vs aumi) so the components are technically distinct, but
    # Pulp namespaces its ObjC view-factory class by manufacturer+subtype --
    # PulpAUCocoaViewFactory_Gnrs_FrgM -- and AU components can share a
    # process. Two plugins defining that class is a duplicate-symbol ODR
    # violation that hands one of them the other's editor.
    PLUGIN_CODE       "FrgR"
    MANUFACTURER_CODE "${FORGE_IDENTITY_AU_MANUFACTURER_CODE}"
)

# VST3's factory macro needs compile-time literals, so the identity arrives as
# defines rather than through descriptor() like every other format.
if(TARGET ForgeModular_VST3)
    target_compile_definitions(ForgeModular_VST3 PRIVATE
        FORGE_PLUGIN_VERSION="${PROJECT_VERSION}"
        FORGE_PLUGIN_NAME="Forge Modular"
        FORGE_PLUGIN_MANUFACTURER="${FORGE_IDENTITY_MANUFACTURER_LITERAL}")
endif()

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
