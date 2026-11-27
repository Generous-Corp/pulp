# test_editor_bridge_forwarders.cmake: EditorBridge's deprecated attachment
# members are templates that forward to pulp/view/editor_bridge_attach.hpp.
# A translation unit that includes that header, or that is built against
# pulp::view-tooling (which defines PULP_EDITOR_BRIDGE_VIEW_ATTACHMENTS and so
# gets it through editor_bridge.hpp), compiles every member call. One that has
# neither must fail to compile, naming the attachment, rather than compile and
# fail to link against a bridge library that defines none of them.
#
# Inputs: CXX (the C++ compiler), PULP_SOURCE_DIR, CHOC_DIR.

foreach(_required CXX PULP_SOURCE_DIR CHOC_DIR)
    if(NOT ${_required})
        message(FATAL_ERROR "test_editor_bridge_forwarders: ${_required} is required")
    endif()
endforeach()

set(_compile "${CXX}" -std=c++20 -fsyntax-only -Wno-deprecated-declarations
    -I${PULP_SOURCE_DIR}/core/editor_bridge/include
    -I${PULP_SOURCE_DIR}/core/view/include
    -I${CHOC_DIR}
    ${PULP_SOURCE_DIR}/test/editor_bridge_forwarders/forward_call.cpp)

execute_process(COMMAND ${_compile} -DPULP_TEST_INCLUDE_ATTACH=1
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "forwarders: the member calls did not compile with editor_bridge_attach.hpp:\n${_out}${_err}")
endif()

# A consumer of pulp::view-tooling gets PULP_EDITOR_BRIDGE_VIEW_ATTACHMENTS and,
# with it, the attachment declarations from editor_bridge.hpp alone.
execute_process(COMMAND ${_compile} -DPULP_EDITOR_BRIDGE_VIEW_ATTACHMENTS=1
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "forwarders: a view-tooling consumer's member calls did not compile:\n${_out}${_err}")
endif()

execute_process(COMMAND ${_compile}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "forwarders: the member calls compiled without editor_bridge_attach.hpp, so a missing header would surface only at link time")
endif()
foreach(_name IN ITEMS attach_webview detach_webview attach_native_runtime detach_native_runtime)
    if(NOT "${_out}${_err}" MATCHES "${_name}")
        message(FATAL_ERROR "forwarders: the compile error does not name ${_name}:\n${_out}${_err}")
    endif()
endforeach()
message(STATUS "forwarders: ok")
