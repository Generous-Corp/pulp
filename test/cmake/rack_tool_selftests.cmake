# Self-tests of the Rack tooling that need no Rack install, no installed
# plugins and no Forge examples directory, so they run on every build rather
# than only where PULP_HAS_RACK is set. Where a Rack install would widen a
# check (test_pack_links, test_prompt_fidelity) the test says so and still
# asserts everything that does not need it.
if(Python3_Interpreter_FOUND)
    add_test(NAME rack-archive-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_archive.py")
    set_tests_properties(rack-archive-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-generate-model-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_generate_model.py")
    set_tests_properties(rack-generate-model-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-intent-context-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_intent_context.py")
    set_tests_properties(rack-intent-context-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-long-horizon-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_long_horizon.py")
    set_tests_properties(rack-long-horizon-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-maker-intent-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_maker_intent.py")
    set_tests_properties(rack-maker-intent-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-measure-ranges-install-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_measure_ranges_install.py")
    set_tests_properties(rack-measure-ranges-install-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-module-activation-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_module_activation.py")
    set_tests_properties(rack-module-activation-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-pack-links-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_pack_links.py")
    set_tests_properties(rack-pack-links-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-portmap-fields-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_portmap_fields.py")
    set_tests_properties(rack-portmap-fields-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-portmap-seed-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_portmap_seed.py")
    set_tests_properties(rack-portmap-seed-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-prompt-fidelity-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_prompt_fidelity.py")
    set_tests_properties(rack-prompt-fidelity-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 600)
    add_test(NAME rack-prompt-handoff-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_prompt_handoff.py")
    set_tests_properties(rack-prompt-handoff-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-patch-lang-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_patch_lang.py")
    set_tests_properties(rack-patch-lang-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-prove-rack-plugin-layout-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_prove_rack_plugin_layout.py")
    set_tests_properties(rack-prove-rack-plugin-layout-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
    add_test(NAME rack-roles-vocabulary-selftest COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/tools/rack/test_roles_vocabulary.py")
    set_tests_properties(rack-roles-vocabulary-selftest PROPERTIES LABELS "rack;selftest" TIMEOUT 120)
endif()
