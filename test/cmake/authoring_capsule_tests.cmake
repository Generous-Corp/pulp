# Authoring-capsule substrate: path admission, canonical JSON and revision
# identity, component completeness, canonical PCM, and the export/open/preview
# admission path.
#
# The suite reaches the module's private canonical-JSON unit, whose header
# exposes the number and serialization rules "so a test can pin the rule
# directly rather than inferring it from a whole envelope". That needs the
# module's own source directory on the include path; the symbols themselves are
# already compiled into the linked library.
if(TARGET pulp::authoring-capsule)
    pulp_add_test_suite(pulp-test-authoring-capsule
        LIBRARIES pulp::authoring-capsule pulp::runtime
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/core/authoring_capsule/src"
        LABELS authoring-capsule
        TIMEOUT 60)
endif()

# The hostile archive corpus: malformed and malicious containers, forged in
# memory by the suite rather than committed as binary fixtures.
#
# It scans the module's own translation units to prove the preview path holds
# no outbound or process-spawning call site, so it needs the module root as a
# definition. That is a structural claim about a code path, which no run of a
# corpus can make on its own: a socket opened on a branch the corpus does not
# reach would still be a network call during preview.
if(TARGET pulp::authoring-capsule)
    pulp_add_test_suite(pulp-test-authoring-capsule-hostile
        LIBRARIES pulp::authoring-capsule pulp::runtime
        COMPILE_DEFINITIONS
            PULP_AUTHORING_CAPSULE_SOURCE_DIR="${CMAKE_SOURCE_DIR}/core/authoring_capsule"
        LABELS authoring-capsule
        TIMEOUT 120)
endif()
