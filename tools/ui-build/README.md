# Pulp UI build package

This directory is the reserved home for the shared DesignIR/UI-IR source writer
and target emitters. The `pulp-package.json` manifest is intentionally present
before the compiler is split out so dependency ownership is reviewable from the
first file and the Vellum boundary lint can keep future modules honest.
