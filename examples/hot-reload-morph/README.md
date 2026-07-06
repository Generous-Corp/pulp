# Hot-Reload Morph — swap a whole plugin (DSP **and** UI) live

This example shows the headline of Pulp's DSP hot-reload: **one reload changes
both the sound and the look.** The logic library ships two versions behind the
*same* parameter contract, so the shell hot-swaps between them with no reload:

| | DSP | Editor |
|---|---|---|
| **WARM** (default) | gentle **sine** tremolo | blue panel, "WARM · sine tremolo" |
| **HARSH** (`-DMORPH_HARSH`) | hard **square** chop | red panel, "HARSH · square chop" |

Reload swaps the DSP (sine wobble → square chop) *and* the editor (blue → red)
in one step — see `ui_warm.png`/`ui_harsh.png` and `warm.wav`/`harsh.wav`
produced by the capture tool below.

## Two trigger paths

1. **Auto file-watch** — recompile the logic and the shell's watcher hot-swaps it
   within ~150 ms. `morph.sh warm` / `morph.sh harsh` copy a prebuilt variant
   over the watched path to flip versions live.
2. **Reload-now** — call `ReloadableShell::reload_now()` (e.g. from a button),
   which forces a re-`dlopen` of the current logic file regardless of mtime. The
   shell also fires `set_on_reloaded()` after each swap so a host can rebuild its
   editor (the UI follows the DSP).

Either way: no reload, no dropout, click-free (the slot crossfades old→new).

## How the UI hot-reloads cleanly: thin logic

The logic builds its *own* editor (`create_view()`), so it needs `pulp::view`.
If it static-linked `pulp::view` there would be **two copies** in the host
process (the host's + the logic's) — duplicate ObjC classes, unsafe. Instead the
logic is built **thin** (`pulp_add_reload_logic(... RESOLVE_FROM_HOST)`): it links
no SDK archives and resolves `pulp::*` from the host at `dlopen`, so there is one
copy of the SDK in the process. The host is marked `pulp_reload_host(<target>)`
to export those symbols. The shell forwards `create_view()` to the active logic,
so the editor reflects the live version.

> **Scope: standalone only.** Thin reload resolves the host's symbols only when
> the host is an **executable** that exports them. A DAW loads a VST3/CLAP
> *bundle* with `RTLD_LOCAL`, so the bundle's symbols aren't in the scope the thin
> logic binds against — in-DAW UI hot-reload via thin logic is unproven/platform-
> dependent and intentionally not shipped here. For **DSP-only** hot-reload in a
> real DAW (REAPER-validated), see [`../hot-reload-demo/`](../hot-reload-demo/),
> which uses the static model and needs no host symbol export.

## Build & prove it

```bash
cmake --build build --target hot-reload-morph-logic-warm hot-reload-morph-logic-harsh \
                            HotReloadMorph_Standalone hot-reload-morph-capture
# Headless A/B proof — renders each version's editor + DSP through a real swap:
./build/examples/hot-reload-morph/hot-reload-morph-capture /tmp/morph
#   /tmp/morph/ui_warm.png  ui_harsh.png   (the editor before/after)
#   /tmp/morph/warm.wav     harsh.wav      (the DSP before/after)
```

## Run it live

```bash
./build/examples/hot-reload-morph/HotReloadMorph    # standalone (audio out the default device)
```
Then `morph.sh harsh` (or edit `logic_morph.cpp` + rebuild) to hear *and* see it
flip — the DSP and the editor swap together.

See [the DSP hot-reload guide](../../docs/guides/dsp-hot-reload.md) for the engine.

## The three trust lanes

The same reload works at three trust levels — you don't change the plugin, only how
you build/ship it (full model: [reload-trust.md](../../docs/guides/reload-trust.md)):

```bash
# 1. Local dev — unsigned, instant. Edit + rebuild; the watcher swaps it. No keys.

# 2. Signed — sign the UX bundle so a consumer verifies it before loading:
pulp ship swap-pack --bundle ui/ --plugin-id com.you.morph   # caps inferred; key in keychain

# 3. Consumer — the signed pack loads only if signature + revocation + version +
#    capabilities all pass; otherwise the running UI/DSP is kept (fail-closed).
```

Opt-out is first-class: ship with reload off, `--allow-unsigned`, or capabilities
wide open — the protection is default-on, never forced.
