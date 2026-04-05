if(PULP_BUILD_WEBVIEW AND NOT PULP_IOS)
    pulp_add_binary_data(pulp_webview_palette_assets
        SOURCES
            web/index.html
            web/palette.css
            web/palette.js
        NAMESPACE pulp_webview_palette_assets
    )

    add_executable(pulp-webview-palette main.cpp)
    target_link_libraries(pulp-webview-palette PRIVATE
        pulp::view
        pulp_webview_palette_assets
    )
    target_compile_features(pulp-webview-palette PRIVATE cxx_std_20)
endif()
