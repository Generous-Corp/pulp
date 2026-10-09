# DSPX-07 installed Audio Unit receipt

Date: 2026-10-08

This receipt records the first production-path AU package and validator run for
the DSPX-07 change. It is tied to PR [#9931](https://github.com/Generous-Corp/pulp/pull/9931)
at head `6790779412bb39625095f37126bffd79f66ebabb`, based on protected `main`
at `9d741c046643ba10fd52f0e1e0dda5cda25a96ce7d` when the package was built.

## Package build

The checkout was configured in Release mode with examples enabled, then built
through the governed builder:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BUILD_EXAMPLES=ON -DPULP_BUILD_TESTS=ON
tools/ci/governed-build.sh cmake --build build --target \
  PulpHostBench_AU PulpHostBench_VST3 PulpHostBench_CLAP PulpHostBench_Standalone
```

All four targets completed successfully. The resulting bundles were present and
were hashed with a deterministic tree hash over sorted relative file names and
file bytes (the method includes each name and byte length before its contents):

| Bundle | Files | Bytes | Tree SHA-256 |
| --- | ---: | ---: | --- |
| `build/AU/PulpHostBench.component` | 8 | 50,936,397 | `090803fe6ce5ac742ecd9459e57b1b3a162017eba4de81f0461feda49878ff7d` |
| `build/VST3/PulpHostBench.vst3` | 8 | 50,770,104 | `59b9d6e1b858e0345043120aff6c3666716227528c090a6ea87a0cfa95d9d0b6` |
| `build/CLAP/PulpHostBench.clap` | 8 | 50,855,710 | `25539219379eef4323f914477dcc507155552b43ff3212447d3587cc42db8e49` |
| `build/examples/host-bench-plugin/PulpHostBench.app` | 7 | 52,438,955 | `5a65888231d636ec92cd843482c811454b8c0dd68c8c1274afa4c4ea7b5ce24e` |

## Installed AU preflight

The AU component was copied to the user component directory after preserving the
previous local fixture as
`~/Library/Audio/Plug-Ins/Components/PulpHostBench.component.pre-dspx-20261008`.
The registrar was restarted before validation.

The executable preflight passed for the installed bundle:

```text
python3 tools/scripts/check_au_component_preflight.py \
  ~/Library/Audio/Plug-Ins/Components/PulpHostBench.component \
  --expect-type aumf --expect-subtype PHBn --expect-manufacturer Pulp \
  --expect-factory PulpHostBenchAUFactory \
  --expect-symbol PulpHostBenchAUFactory --check-permissions
```

The preflight proved the installed `Info.plist`, `aumf` type, `PHBn` subtype,
`Pulp` manufacturer, factory function, factory symbol, executable path, and
permissions. The installed component's deterministic tree SHA-256 was
`090803fe6ce5ac742ecd9459e57b1b3a162017eba4de81f0461feda49878ff7d`, matching
the built AU bundle.

## Targeted AU validation

The installed component passed the Apple validator twice:

```text
/usr/bin/auval -v aumf PHBn Pulp
```

Both runs returned exit code 0 and printed `AU VALIDATION SUCCEEDED.`. Each run
covered AU v2 open, initialize, default scopes, required and recommended
properties, parameter persistence, channel negotiation, render tests at
multiple sample rates and block sizes, bad-max-frame rejection, parameter
scheduling, and MIDI dispatch.

The broad inventory command `auval -a` did not finish within a 10-second
diagnostic timeout on this machine. It emitted no completed inventory, so this
receipt does not claim a full registrar inventory. The targeted validator result
is the authoritative discovery and runtime proof for `aumf PHBn Pulp`.

## Host-lab disposition

| Surface | Status | Evidence or reason |
| --- | --- | --- |
| AU bundle metadata and install | **PASS** | Installed tree matches the built bundle and preflight passes. |
| AU v2 targeted `auval` discovery and render | **PASS** | Two independent targeted runs returned `AU VALIDATION SUCCEEDED.` |
| AU v2 code-signing and Gatekeeper | **NOT CLAIMED** | The local build is unsigned; `codesign --verify --deep --strict` reports an unsigned/resource signature failure. A Developer ID signing and notarization run remains required for release distribution. |
| Logic Pro DAW session | **NOT TRIGGERED** | Logic is not available in this automated session; follow `docs/validation/daw-bench/01-logic-pro-au.md`. |
| AUM/AUv3 physical host | **NOT TRIGGERED** | Requires an iOS device and AUM; follow `docs/validation/daw-bench/08-aum-auv3.md`. |
| AUv3 packaged extension | **NOT CLAIMED** | This package lane builds AU v2 HostBench. The DSPX lifecycle fixture separately exercises the AUv3 render block; an installed AUv3 host run remains open. |

This receipt closes the installed AU v2 discovery/runtime gap for the bounded
baked-graph adapter. It does not claim signed distribution, Logic/AUM behavior,
or commercial host compatibility.
