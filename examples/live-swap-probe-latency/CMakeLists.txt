# P0.5 PROBE plugin (Phase 0 derisk — NOT for main). A latency-flipping effect
# used to observe host PDC renegotiation on a runtime flag_latency_changed().
# Header-only processor (no SOURCES -> INTERFACE Core); entry files are picked up
# by convention (clap_entry.cpp / vst3_entry.cpp).
pulp_add_plugin(LatencyProbe
    FORMATS           VST3 CLAP Standalone
    PLUGIN_NAME       "Pulp Latency Probe"
    BUNDLE_ID         "com.pulp.examples.latency-probe"
    MANUFACTURER      "Pulp"
    VERSION           "0.1.0"
    CATEGORY          Effect
    PLUGIN_CODE       "LtP1"
    MANUFACTURER_CODE "Pulp"
)
