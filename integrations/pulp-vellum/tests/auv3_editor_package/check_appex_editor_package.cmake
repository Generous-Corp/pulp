# Reads the symbol tables of a built AUv3 framework and appex stub.
#
#   -DNM=<nm> -DFRAMEWORK=<framework binary> -DAPPEX=<appex binary>
#
# The Vellum editor's lifecycle hooks and the generated editor-package
# instantiation must be defined in the framework, and the appex must define
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

# Local and weak symbols too: the editor template instantiation is not a
# global definition.
function(_all_symbols image out)
    execute_process(COMMAND "${NM}" -C "${image}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "nm failed on ${image}: ${_error}")
    endif()
    set(${out} "${_output}" PARENT_SCOPE)
endfunction()

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

# The generated editor-package source instantiates the editor template. That
# instantiation exists once, in the framework.
set(_instance "pulp::vellum::Editor<pulp::test_fixtures::Auv3EditorView>")
string(FIND "${framework_all}" "${_instance}" _pos)
if(_pos EQUAL -1)
    message(FATAL_ERROR "the AUv3 framework carries no ${_instance}")
endif()
string(FIND "${appex_all}" "${_instance}" _pos)
if(NOT _pos EQUAL -1)
    message(FATAL_ERROR "the AUv3 appex stub carries ${_instance}; the editor-package source belongs to the framework")
endif()

message(STATUS "vellum-auv3-appex-editor-package: ok")
