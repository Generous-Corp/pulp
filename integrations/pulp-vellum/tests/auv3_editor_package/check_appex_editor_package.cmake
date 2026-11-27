# Reads the symbol tables of a built AUv3 framework and appex stub.
#
#   -DNM=<nm> -DFRAMEWORK=<framework binary> -DAPPEX=<appex binary>
#
# The Vellum editor's lifecycle hooks and the generated editor-package
# registrar must be defined in the framework, and the appex must define
# neither: it only loads the framework.

foreach(_var NM FRAMEWORK APPEX)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "${_var} is required")
    endif()
endforeach()
foreach(_image "${FRAMEWORK}" "${APPEX}")
    if(NOT EXISTS "${_image}")
        message(FATAL_ERROR "not built: ${_image}")
    endif()
endforeach()

function(_defined_symbols image out)
    execute_process(COMMAND "${NM}" -gU -C "${image}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "nm failed on ${image}: ${_error}")
    endif()
    set(${out} "${_output}" PARENT_SCOPE)
endfunction()

# Local symbols too: the registrar and its hook have internal linkage.
function(_all_symbols image out)
    execute_process(COMMAND "${NM}" -C "${image}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "nm failed on ${image}: ${_error}")
    endif()
    set(${out} "${_output}" PARENT_SCOPE)
endfunction()

_defined_symbols("${FRAMEWORK}" framework_defined)
_all_symbols("${FRAMEWORK}" framework_all)
_all_symbols("${APPEX}" appex_all)

set(editor_hooks editor_opened editor_idle editor_closing)
foreach(_hook IN LISTS editor_hooks)
    set(_symbol "pulp::test_fixtures::Auv3EditorView::${_hook}()")
    string(FIND "${framework_all}" "${_symbol}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR "the AUv3 framework does not define ${_symbol}")
    endif()
    string(FIND "${appex_all}" "${_symbol}" _pos)
    if(NOT _pos EQUAL -1)
        message(FATAL_ERROR "the AUv3 appex stub defines ${_symbol}; the editor code belongs to the framework")
    endif()
endforeach()

# The generated source's static registrar exists once, in the framework.
foreach(_image framework appex)
    string(REGEX MATCHALL "pulp_plugin_editor_factory_registrar" _registrars "${${_image}_all}")
    list(LENGTH _registrars _count)
    if(_image STREQUAL "framework" AND _count EQUAL 0)
        message(FATAL_ERROR "the AUv3 framework carries no editor-package registrar")
    endif()
    if(_image STREQUAL "appex" AND NOT _count EQUAL 0)
        message(FATAL_ERROR "the AUv3 appex stub carries the editor-package registrar")
    endif()
endforeach()

message(STATUS "vellum-auv3-appex-editor-package: ok")
