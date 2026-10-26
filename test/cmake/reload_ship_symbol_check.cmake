# Ship-safety symbol-absence check for the reload dev watcher. Scans the two
# fixture binaries against the `reload-ship` gate of
# tools/cmake/release_artifact_forbidden.json: the dev build must contain every
# marker of the gate (positive control, so the marker cannot silently rot into
# a vacuous pass); the gate-off ship build must contain none.
include("${PULP_SOURCE_DIR}/tools/cmake/PulpReleaseArtifactScan.cmake")
pulp_release_artifact_load()
pulp_release_artifact_require(ARTIFACT "${DEV_BIN}" GATES reload-ship
    LABEL "ship-safety: reload watcher present in the dev fixture")
pulp_release_artifact_forbid(ARTIFACT "${SHIP_BIN}" GATES reload-ship
    LABEL "ship-safety: reload watcher absent from the ship (PULP_RELOAD_DEV_WATCHER=0) fixture")
