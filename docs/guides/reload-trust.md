# Reload Trust & Safety

Pulp can hot-reload a plugin's DSP and UI — live, in a DAW, without reopening the
editor. This guide explains how that stays safe to ship to end users while remaining
zero-friction for developers, and how to opt in, opt out, and manage keys.

The one-line model: **signing proves *who* shipped the code and that it wasn't
tampered with; capabilities bound *what* it can do. Signing is not a sandbox** —
it's provenance plus a blast-radius cap. Everything below follows from that.

## The dial: convenience ↔ security

Three lanes, chosen by build/config — you don't rewrite your plugin to move between
them:

1. **Local dev (default in dev builds): unsigned, instant.** You (or an agent) edit
   UX/DSP and it hot-reloads immediately. No keys, no signing. Trust boundary =
   "your own machine." The dev watcher is compiled out of shipping builds.
2. **Protected dev (opt-in, default OFF): signed even in dev.** For an open or
   collaborative project where you hold a signing key and don't want a teammate,
   agent, or CI dropping unsigned code into a shared dev build, set
   `reload.require_signed = true`. Same verify path as consumers, applied in dev.
3. **Consumer (default in shipping builds): signed, verified, capability-gated.** A
   reload artifact loads only if, in order: its signature matches the pinned key →
   it isn't revoked → it isn't a downgrade → its declared capabilities ⊆ what the
   host granted. Otherwise it is refused (fail-closed) and the current DSP/UI keeps
   running. No crash.

## What each layer protects against

- **Code loaded from an untrusted path.** The one genuinely new surface hot-reload
  adds is *code arriving after ship*. Consumers load only **signed** packs, verified
  against a **pinned key**, **before any `dlopen`** — so a rejected image's
  constructors never run, and a symlink/path detour to unverified bytes is refused.
- **"A plugin opened a network endpoint."** Network/filesystem/clipboard/etc. are
  **declared capabilities**: a UI granted no `network` can't open a socket, signed or
  not. Capabilities are enforced at the JS bridge (native code is provenance-only —
  it can't be sandboxed). This is the blast-radius cap if a key is ever misused.
- **A leaked signing key.** A **signed revocation list** kills a compromised key
  (monotonic, offline-safe, fail-closed) without re-shipping every consumer.
- **A tampered / MITM'd delivery.** Per-file integrity hashes + the signature; the
  installed bytes are re-verified and the loader reads only the immutable installed
  copy.

## Not nannies — opt-out is first-class

The protection is default-on for consumers, never hard-required. You can ship:

- **with reload disabled entirely** (an ordinary static plugin — none of this
  applies),
- **with reload enabled but unsigned** (an explicit, loud `--allow-unsigned` escape;
  consumers then get no provenance/tamper protection, but it's allowed), or
- **with capabilities wide open.**

Disabled is not the default, but it's a first-class choice. Trust is opt-in at the
API — the loader takes an optional trust argument and the bridge defaults to
full-featured; shipping builds flip those defaults on. We provide safe defaults and
the escape hatches; we don't force a posture.

## Distributed and offline — no service required

Verification is local Ed25519 math against a key baked into the (platform-signed)
plugin. Signing is a local/hardware key operation. There is **no Pulp service** in
the trust path: no signing server, no key server, no update backend. That's cheaper
to run and *safer* — there's no central system whose compromise would push malware
to everyone. Revocation ships as a signed list; remote UX updates (opt-in) pull a
signed pack from a plain static host and verify it locally.

### Remote UX updates (opt-in, offline-first)

`check_and_fetch_remote_ux_update()` (in `reload/remote_update_fetch.hpp`) is the
network layer over the pure gate. It is **off by default**; when enabled it fetches a
signed pack over **HTTPS** and applies it only if the gate accepts (signature →
not-revoked → not-a-downgrade → capabilities ⊆ granted), then installs it
content-addressed so the loader reads immutable, re-verified bytes.

- **Offline is a normal outcome, never a failure.** An unreachable host, timeout, or
  non-2xx returns `Unavailable` — the plugin just keeps its current UI; nothing blocks
  or throws. Supply an `offline_fallback_root` and that pack is surfaced so the plugin
  keeps *your* look with no network.
- **Bring your own transport.** The `fetcher` is injectable, so any delivery style
  works — a CDN, an object store, an authed server. `make_http_pack_fetcher()` is a
  convenience default over `runtime::http_*`.
- **Privacy.** An update check sends only what the server needs to choose a pack —
  plugin id, channel, installed version — over HTTPS. No device id, no user id, no
  telemetry (`build_update_check_url()` is pure, so that contract is testable).

### Enforcing "signed only" in the dev watcher

Setting `reload.require_signed = true` is enforced by the dev watcher itself: before
staging a changed logic file it resolves the signed sidecar manifest and verifies the
watched bytes against the pinned key **before any load**, refusing fail-closed if the
sidecar is missing or invalid (`ReloadableShell::set_reload_trust_policy`). The default
(OFF) is the frictionless unsigned dev loop.

## Signing a pack

```
pulp ship swap-pack --bundle ui/ --plugin-id com.you.synth
```

- Capabilities are **inferred from your UI's JS** — you don't hand-maintain the list.
- It prints a summary of exactly what will be signed and asks you to confirm
  (`--yes` for CI).
- The signing key comes from your **macOS keychain** (created on first use) or a
  `--sign-key <file>`.

## Keys — no surprises

If Pulp generates a key you didn't already have, it says so loudly: what it created,
**where it lives**, and the consequences (lose the signing key → you can't ship
updates; lose the offline revocation key → you can't revoke a leaked one; leak either
→ someone can sign as you). Pulp reuses a key it finds and **never silently
regenerates** one (a new key is a new identity consumers reject).

- **Storage.** The macOS keychain by default (survives a repo delete), under a
  stable, discoverable name (`pulp.reload.signing.<plugin_id>`). Optionally back it
  up as a GitHub Actions secret in your **plugin's own repo** with
  `--backup-github` (refused for the core Pulp repo) so CI can sign and it survives
  machine loss. The **public** key is safe to commit (it's baked into the plugin);
  only the **private** key needs protecting.
- **Two keys.** A hot signing key (used often) and an offline revocation key (used
  rarely, kept safer). Both are named + versioned so they're easy to find, import,
  rotate, and revoke; rotation writes a new generation rather than overwriting, so
  the old key stays importable during a transition.

See also: [dsp-hot-reload.md](dsp-hot-reload.md) and
[live-swap-cpu-gpu.md](../reference/live-swap-cpu-gpu.md) for the reload engine itself, and
[shipping.md](shipping.md) for platform signing/notarization.
