if(PULP_BUILD_WEBVIEW)
    pulp_add_plugin(PulpWebViewPlugin
        FORMATS             VST3 AU CLAP Standalone
        PLUGIN_NAME         "PulpWebViewPlugin"
        BUNDLE_ID           "com.pulp.webview-plugin"
        MANUFACTURER        "Pulp"
        VERSION             "1.0.0"
        CATEGORY            Effect
        PLUGIN_CODE         "PWvp"
        MANUFACTURER_CODE   "Pulp"
    )

    if(TARGET PulpWebViewPlugin_Standalone)
        pulp_app_icon(PulpWebViewPlugin_Standalone
            SOURCE assets/app-icon.png
        )
    endif()
endif()
