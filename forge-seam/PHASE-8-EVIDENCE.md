# Phase 8 — validate, sign, ship

Every result below was produced by running the thing, not by reading code.
Re-run after signing, because signing changes the binary and a result that
predates the binary it describes is not a result.

## Formats

| Format | Built | Validated | REAPER editor-open |
|---|---|---|---|
| AU (`aufx FrgR Gnrs`) | yes | `auval` — AU VALIDATION SUCCEEDED | PASS |
| VST3 | yes | load-probe resolves `GetPluginFactory` + `bundleEntry` | PASS |
| CLAP | yes | `clap-validator` 33/44 (2 known, see below) | PASS |
| Standalone | yes | runs, reports `generator ready` | n/a |

## Sign / notarize / staple

- All four signed with Developer ID, `--options runtime --timestamp`, inner
  dylibs first (each bundle carries `libwgpu_native.dylib`).
- Notarization **Accepted** (`a7845079-8065-4605-bd25-9bc2bd198c7b`).
- Stapled: "The staple and validate action worked!"
- Gatekeeper: `accepted`, `source=Notarized Developer ID`.

## The two CLAP failures are not ours

`param-set-events` and `param-set-no-cookies`, both "after calling
`clap_plugin_params::flush()`, the parameter values did not change". This is a
gap in the shared Pulp CLAP adapter, not something Forge Modular introduced:
Forge FX, built from the same tree, fails **three** of the same kind. Recorded
rather than hidden — a validator result nobody reads is the same as not running
it.

## Two defects validation caught that reading would not have

**An AU bundle with no binary in it.** The first build failed, but the
`.component` directory existed from the attempt, and treating that as success
nearly shipped an empty component — it was briefly installed before `nm` showed
no exported factory. A directory is not an artifact.

**A four-character code collision.** Forge MIDI already owns `FrgM`, and Forge
Modular claimed it too. The AU types differ (`aufx` vs `aumi`) so the components
are technically distinct, and every scan-level check passes. But Pulp namespaces
its ObjC view-factory class by manufacturer+subtype —
`PulpAUCocoaViewFactory_Gnrs_FrgM` — and AU components can share a process. Two
plugins defining that class is a duplicate-symbol ODR violation that hands one
of them the other's editor. Forge Modular is now `FrgR`, confirmed distinct by
`nm`.

## Housekeeping

A stale `Forge Modular.clap` dated 29 Jul — an old JS-shell build predating this
work — was sitting in `~/Library/Audio/Plug-Ins/CLAP/` where DAWs scan. Replaced
with the signed, validated build.
