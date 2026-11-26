
## Optional scripted bridge

`PULP_ENABLE_PULP_VELLUM_SCRIPTED` controls the separate scripted editor target and defaults to `ON`. Native Vellum editor packages only require `pulp::vellum` and `Vellum::MirrorView_core`, so an SDK consumer can set the option `OFF` when its installed Vellum SDK does not export the matching `ScriptedUiSession` constructor. This keeps native Vellum editors buildable while the scripted bridge is upgraded independently.
