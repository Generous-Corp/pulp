# PulpUtils CMake split — B4 receipt

Branch: `codex/refactor-b4-pulputils-20261002`
Base: `origin/main` at `064b045cbb`
Implementation: `0224a2b710`

The governed Release build completed and `cmake --install build` succeeded. The
installed SDK compatibility consumer verifies the shim's 15 public commands,
all relative helper includes, and the stable target receipt for
`Pulp::audio`, `Pulp::format`, `Pulp::midi`, `Pulp::standalone`, and `Pulp::view`.

The focused install/consumer suite passed 6/6:

- `cmake-au-v2-type-selection`
- `cmake-plugin-runtime-manifest-layout`
- `cmake-pulp-install-layout`
- `cmake-pulp-install-format-sources`
- `cmake-pulp-install-midi-tuning-sources`
- `cmake-pulp-utils-compat`

The pre-edit configure and target captures were taken before the split. The
first pre-edit install attempt was incomplete because the mbedtls Everest
archive had not been built; the post-edit governed build produced a successful
install. Format-specific target differences are attributable to VST3/AU SDK
availability between captures; common target names remained stable.

State intentionally retained in `PulpUtils.cmake`: shared `_PULP_*` target
aliases and source probes, `_pulp_apply_view_mac_objc_suffix`, and the
cross-concern `pulp_add_plugin` / `pulp_add_plugin_bundle` dispatchers with their
`PULP_${target}_*` state. The extracted modules are `PulpUiAssets.cmake`,
`PulpInstall.cmake`, and `PulpReload.cmake`; existing format/app modules remain
transitively included by the shim.
