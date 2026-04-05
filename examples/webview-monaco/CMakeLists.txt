set(PULP_MONACO_BUNDLE_DIR "" CACHE PATH
    "Path to a browser-ready Monaco bundle for the optional WebView example")

if(PULP_BUILD_WEBVIEW AND NOT PULP_IOS)
    if(PULP_MONACO_BUNDLE_DIR AND EXISTS "${PULP_MONACO_BUNDLE_DIR}/index.html")
        add_executable(pulp-webview-monaco main.cpp)
        target_link_libraries(pulp-webview-monaco PRIVATE pulp::view)
        target_compile_features(pulp-webview-monaco PRIVATE cxx_std_20)
        target_compile_definitions(pulp-webview-monaco PRIVATE
            PULP_MONACO_BUNDLE_DIR="${PULP_MONACO_BUNDLE_DIR}"
        )
    else()
        message(STATUS "Pulp WebView Monaco example skipped (set PULP_MONACO_BUNDLE_DIR to a built bundle directory)")
    endif()
endif()
