pulp_add_plugin(PulpIsolationA
    NO_EDITOR
    FORMATS           ${PULP_ISOLATION_FORMATS}
    PLUGIN_NAME       "Pulp Isolation A"
    BUNDLE_ID         "dev.pulp.test.isolation-a"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PIsA"
    MANUFACTURER_CODE "PlIs"
    SOURCES           ../isolation_processor.hpp
)
