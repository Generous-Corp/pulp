if(NOT NATIVE_STANDALONE OR NOT NATIVE_CLAP OR
   NOT WEBKIT_POSITIVE_CONTROL OR NOT JSC_POSITIVE_CONTROL)
    message(FATAL_ERROR "native link-floor scanner requires all artifact paths")
endif()

foreach(_artifact
        "${NATIVE_STANDALONE}"
        "${NATIVE_CLAP}"
        "${WEBKIT_POSITIVE_CONTROL}"
        "${JSC_POSITIVE_CONTROL}")
    if(NOT EXISTS "${_artifact}")
        message(FATAL_ERROR "native link-floor artifact does not exist: ${_artifact}")
    endif()
endforeach()

function(_otool out artifact)
    execute_process(
        COMMAND /usr/bin/otool -L "${artifact}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "otool failed for ${artifact}: ${_error}")
    endif()
    set(${out} "${_output}" PARENT_SCOPE)
endfunction()

_otool(_webkit_positive_deps "${WEBKIT_POSITIVE_CONTROL}")
if(NOT _webkit_positive_deps MATCHES "WebKit.framework")
    message(FATAL_ERROR
        "positive control does not link WebKit; the absence scanner is not mutation-sensitive:\n${_webkit_positive_deps}")
endif()

_otool(_jsc_positive_deps "${JSC_POSITIVE_CONTROL}")
if(NOT _jsc_positive_deps MATCHES "JavaScriptCore.framework")
    message(FATAL_ERROR
        "positive control does not link JavaScriptCore; the absence scanner is not mutation-sensitive:\n${_jsc_positive_deps}")
endif()

# wgpu-native positive control. The native artifacts render through Skia
# Graphite on Skia's static Dawn and must not load libwgpu_native: when two
# plug-ins share a host, dyld shares one copy per process by install name.
if(WGPU_POSITIVE_CONTROL)
    if(NOT EXISTS "${WGPU_POSITIVE_CONTROL}")
        message(FATAL_ERROR "wgpu positive control does not exist: ${WGPU_POSITIVE_CONTROL}")
    endif()
    _otool(_wgpu_positive_deps "${WGPU_POSITIVE_CONTROL}")
    if(NOT _wgpu_positive_deps MATCHES "libwgpu_native")
        message(FATAL_ERROR
            "positive control does not load libwgpu_native; the absence scanner is not mutation-sensitive:\n${_wgpu_positive_deps}")
    endif()
endif()

foreach(_artifact "${NATIVE_STANDALONE}" "${NATIVE_CLAP}")
    _otool(_deps "${_artifact}")
    if(_deps MATCHES "WebKit.framework")
        message(FATAL_ERROR "native artifact links forbidden WebKit: ${_artifact}\n${_deps}")
    endif()
    if(EXPECT_JSC)
        if(NOT _deps MATCHES "JavaScriptCore.framework")
            message(FATAL_ERROR "JSC-selected native artifact lost JavaScriptCore: ${_artifact}\n${_deps}")
        endif()
    elseif(_deps MATCHES "JavaScriptCore.framework")
        message(FATAL_ERROR "QuickJS native artifact links forbidden JavaScriptCore: ${_artifact}\n${_deps}")
    endif()
    if(_deps MATCHES "libwgpu_native")
        message(FATAL_ERROR
            "native artifact loads wgpu-native beside Skia's Dawn: ${_artifact}\n${_deps}")
    endif()

    execute_process(
        COMMAND /usr/bin/nm -C "${_artifact}"
        RESULT_VARIABLE _nm_status
        OUTPUT_VARIABLE _symbols
        ERROR_VARIABLE _nm_error)
    if(NOT _nm_status EQUAL 0 OR NOT _symbols MATCHES "ScriptedUiSession")
        message(FATAL_ERROR
            "native artifact lost ScriptedUiSession symbols: ${_artifact}\n${_nm_error}")
    endif()
    # The GPU path must still be there: an artifact with no Dawn at all would
    # pass the wgpu-native absence check for the wrong reason.
    if(NOT _symbols MATCHES "dawn::native::")
        message(FATAL_ERROR "native artifact lost Skia's Dawn (dawn::native): ${_artifact}")
    endif()
    if(_symbols MATCHES "WebViewPanel|WKWebView|choc::ui::WebView")
        message(FATAL_ERROR
            "native artifact contains forbidden WebView symbols: ${_artifact}")
    endif()

    get_filename_component(_artifact_dir "${_artifact}" DIRECTORY)
    file(GLOB_RECURSE _resources LIST_DIRECTORIES false "${_artifact_dir}/*")
    if("${_resources}" MATCHES "libwgpu_native")
        message(FATAL_ERROR
            "native artifact staged libwgpu_native beside its executable: ${_artifact_dir}\n${_resources}")
    endif()
    if("${_resources}" MATCHES "WebKit|WebView")
        message(FATAL_ERROR
            "native artifact staged forbidden WebView resources: ${_artifact_dir}\n${_resources}")
    endif()
endforeach()

if(EXPECT_JSC)
    set(_engine_contract "JavaScriptCore present")
else()
    set(_engine_contract "JavaScriptCore absent")
endif()
message(STATUS
    "native link floor verified: ScriptedUiSession + Dawn present; wgpu-native and WebKit absent; ${_engine_contract}")
