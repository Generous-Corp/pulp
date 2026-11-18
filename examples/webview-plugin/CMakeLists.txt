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

    # The WebView is not part of the pulp::view umbrella on Linux, so the
    # plug-in links it itself.
    foreach(_fmt VST3 AU CLAP Standalone)
        if(TARGET PulpWebViewPlugin_${_fmt})
            target_link_libraries(PulpWebViewPlugin_${_fmt} PRIVATE pulp::view-webview)
        endif()
    endforeach()

    if(TARGET PulpWebViewPlugin_Standalone)
        pulp_app_icon(PulpWebViewPlugin_Standalone
            SOURCE assets/app-icon.png
        )
    endif()
endif()
