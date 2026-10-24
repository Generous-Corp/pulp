pulp_add_plugin(PulpIsolationB
    NO_EDITOR
    FORMATS           ${PULP_ISOLATION_FORMATS}
    PLUGIN_NAME       "Pulp Isolation B"
    BUNDLE_ID         "dev.pulp.test.isolation-b"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Effect
    PLUGIN_CODE       "PIsB"
    MANUFACTURER_CODE "PlIs"
    SOURCES           ../isolation_processor.hpp
)
