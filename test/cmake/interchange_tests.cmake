# Format-neutral interchange machinery: the concept vocabulary, the generated
# per-format capability tables, the census walker over a document, and the
# plan-then-run export contract. Format adapters register their own tests with
# their own subsystem; nothing format-specific belongs here.

pulp_add_test_suite(pulp-test-interchange
    SOURCES test_interchange_capability.cpp
        test_interchange_census.cpp
        test_interchange_export_plan.cpp
    LIBRARIES pulp::interchange pulp::timeline)

if(Python3_Interpreter_FOUND)
    set(PULP_CAPABILITY_EMIT
        "${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/core/interchange/tools/capability_emit.py")

    # Capability drift gates: each committed artifact must match a fresh
    # emission from core/interchange/capabilities/*.json. The three projections
    # -- the C++ vocabulary, the C++ tables, and the docs page -- share one
    # generator, so a JSON edit that reaches only some of them is drift.
    add_test(NAME interchange-concepts-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact
                ${CMAKE_SOURCE_DIR}/core/interchange/include/pulp/interchange/generated/concepts.hpp
            --emit-cmd "${PULP_CAPABILITY_EMIT} --emit concepts")

    add_test(NAME interchange-tables-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact
                ${CMAKE_SOURCE_DIR}/core/interchange/include/pulp/interchange/generated/capability_tables.hpp
            --emit-cmd "${PULP_CAPABILITY_EMIT} --emit tables")

    add_test(NAME interchange-matrix-docs-drift
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/schema_drift_check.py
            --artifact ${CMAKE_SOURCE_DIR}/docs/reference/interchange-matrix.md
            --emit-cmd "${PULP_CAPABILITY_EMIT} --emit docs")

    # Paired self-test: proves a fresh emission is complete and that the loader
    # rejects capability data the tables could not represent honestly.
    add_test(NAME interchange-capability-emit-selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/core/interchange/tools/test_capability_emit.py)
endif()
