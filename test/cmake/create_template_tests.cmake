# `pulp create` plugin templates, materialized and built as ordinary tests.
# Included by test/cmake/cli_tests.cmake.
#
# New projects copy these templates verbatim, so a template that does not
# compile, or whose generated test fails, ships a broken first build to every
# user who scaffolds that type. Each plugin template's processor header and
# test are expanded here with fixed placeholder values and compiled against
# the in-tree SDK, the same way the generated CMakeLists.txt builds them.

set(_pulp_create_template_types gain effect app instrument bare)
set(_pulp_create_template_root "${CMAKE_SOURCE_DIR}/tools/templates")
set(_pulp_create_template_out "${CMAKE_BINARY_DIR}/test/create-templates")

function(_pulp_expand_create_template type in_file out_file)
    set(class "TplCheck${type}")
    set(lower "tplcheck_${type}")
    file(READ "${in_file}" content)
    foreach(pair
            "PLUGIN_NAME=${class}"
            "CLASS_NAME=${class}"
            "LOWER_NAME=${lower}"
            "NAMESPACE=${lower}"
            "FACTORY_NAME=${lower}"
            "HEADER_NAME=${lower}.hpp"
            "TARGET_NAME=${class}"
            "MANUFACTURER=Pulp"
            "MANUFACTURER_CODE=Pulp"
            "BUNDLE_ID=com.pulp.${lower}"
            "VERSION=1.0.0"
            "PLUGIN_CODE=Tplc"
            "AAX_PRODUCT_CODE=TplC"
            "AAX_NATIVE_CODE=Tplc"
            "FORMATS=CLAP"
            "DESCRIPTION=create template check"
            "PLUGIN_URI=urn:pulp:${lower}"
            "VST3_UID=00000000000000000000000000000000"
            "SDK_VERSION=0.0.0")
        string(FIND "${pair}" "=" eq)
        string(SUBSTRING "${pair}" 0 ${eq} key)
        math(EXPR value_start "${eq} + 1")
        string(SUBSTRING "${pair}" ${value_start} -1 value)
        string(REPLACE "{{${key}}}" "${value}" content "${content}")
    endforeach()
    # Rewrite only on change so a reconfigure does not force a rebuild.
    set(existing "")
    if(EXISTS "${out_file}")
        file(READ "${out_file}" existing)
    endif()
    if(NOT existing STREQUAL content)
        file(WRITE "${out_file}" "${content}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${in_file}")
endfunction()

foreach(_type IN LISTS _pulp_create_template_types)
    set(_src_dir "${_pulp_create_template_root}/${_type}")
    set(_out_dir "${_pulp_create_template_out}/${_type}")
    file(MAKE_DIRECTORY "${_out_dir}")
    _pulp_expand_create_template(${_type}
        "${_src_dir}/processor.hpp.template" "${_out_dir}/tplcheck_${_type}.hpp")
    _pulp_expand_create_template(${_type}
        "${_src_dir}/test.cpp.template" "${_out_dir}/test_tplcheck_${_type}.cpp")

    set(_target pulp-test-create-template-${_type})
    add_executable(${_target} "${_out_dir}/test_tplcheck_${_type}.cpp")
    target_include_directories(${_target} PRIVATE "${_out_dir}")
    target_link_libraries(${_target} PRIVATE pulp::format Catch2::Catch2WithMain)
    catch_discover_tests(${_target}
        TEST_PREFIX "create-template-${_type}: "
        PROPERTIES LABELS "cli;create-template")
endforeach()
