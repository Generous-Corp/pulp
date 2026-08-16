if(NOT NATIVE_STANDALONE OR NOT NATIVE_CLAP OR NOT POSITIVE_CONTROL)
    message(FATAL_ERROR "native link-floor scanner requires all artifact paths")
endif()

foreach(_artifact "${NATIVE_STANDALONE}" "${NATIVE_CLAP}" "${POSITIVE_CONTROL}")
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

_otool(_positive_deps "${POSITIVE_CONTROL}")
if(NOT _positive_deps MATCHES "WebKit.framework")
    message(FATAL_ERROR
        "positive control does not link WebKit; the absence scanner is not mutation-sensitive:\n${_positive_deps}")
endif()

foreach(_artifact "${NATIVE_STANDALONE}" "${NATIVE_CLAP}")
    _otool(_deps "${_artifact}")
    if(_deps MATCHES "WebKit.framework")
        message(FATAL_ERROR "native artifact links forbidden WebKit: ${_artifact}\n${_deps}")
    endif()
    if(NOT _deps MATCHES "JavaScriptCore.framework")
        message(FATAL_ERROR "native artifact lost JavaScriptCore: ${_artifact}\n${_deps}")
    endif()
    if(NOT _deps MATCHES "libwgpu_native.dylib")
        message(FATAL_ERROR "native artifact lost wgpu-native: ${_artifact}\n${_deps}")
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
    if(_symbols MATCHES "WebViewPanel|WKWebView|choc::ui::WebView")
        message(FATAL_ERROR
            "native artifact contains forbidden WebView symbols: ${_artifact}")
    endif()

    get_filename_component(_artifact_dir "${_artifact}" DIRECTORY)
    file(GLOB_RECURSE _resources LIST_DIRECTORIES false "${_artifact_dir}/*")
    if(NOT "${_artifact_dir}/libwgpu_native.dylib" IN_LIST _resources)
        message(FATAL_ERROR
            "native artifact did not stage libwgpu_native.dylib beside its executable: ${_artifact_dir}")
    endif()
    if("${_resources}" MATCHES "WebKit|WebView")
        message(FATAL_ERROR
            "native artifact staged forbidden WebView resources: ${_artifact_dir}\n${_resources}")
    endif()
endforeach()

message(STATUS
    "native link floor verified: ScriptedUiSession + JavaScriptCore + wgpu present; WebKit absent")
