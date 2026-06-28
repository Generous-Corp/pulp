# Non-goals

> Scope boundaries. Things we have **deliberately decided not to pursue**
> in Pulp, with rationale. Not "we haven't gotten to it yet" — these are
> settled decisions that change only with explicit revisit.

If something on this list later becomes strategic, the revisit goes
through a planning-doc proposal + VISION.md update before a GitHub
issue is filed.

---

## Ruled out of scope

### Video playback and capture

Pulp is an audio-first framework. Video decode/encode, camera capture,
live-stream ingest, and frame-level video effects are not in scope.

- **Why:** Video stacks are their own ecosystem (H.264/HEVC codecs,
  hardware decoders, color-space pipelines, YUV↔RGB conversion).
  Bundling them would double the surface area and the licensing
  complexity for a feature set outside Pulp's audio-plugin mission.
- **If you need it:** Drop AVFoundation / MediaFoundation / GStreamer
  into the host app directly. Pulp won't abstract them.

### In-app purchases, marketplace, product unlocking

Pulp does not provide a production IAP / product-key / marketplace
layer. The `pulp::events::IapClient` API is only a host-overridable
stub and mock seam: Pulp's built-in backend reports `Unavailable`
everywhere and never performs a purchase. Plugin developers who sell
commercial products handle billing and license validation through their
own stack.

- **Why:** App-store IAP and license-server integration are
  business-domain decisions (receipt format, fraud detection, refund
  handling) that change per product. Pulp ships the crypto primitives
  (`core/runtime/src/crypto.cpp`) that a license layer can use, but
  the policy layer stays out of scope.
- **If you need it:** RevenueCat, Paddle, Gumroad, self-hosted
  Ed25519-signed license keys, or StoreKit directly.

### Audio CD reading and burning

- **Why:** CD drives are largely gone from modern Mac/Windows/Linux
  hardware, and no mobile platform exposes optical-disc access.
  Targeting the medium is a declining-returns effort.
- **If you need it:** Use platform-native CD tools (cdparanoia, dBpoweramp,
  macOS `drutil`) outside the Pulp pipeline and pass decoded audio in.

### Bluetooth MIDI pairing UI

- **Why:** macOS/iOS/Android all ship their own system Bluetooth MIDI
  pairing surfaces (Audio MIDI Setup, Settings → Bluetooth, MIDI BLE
  scanner apps) that users already know. Duplicating that inside Pulp
  would split the pairing story per-app and confuse users.
- **What we DO support:** Once a BLE MIDI device is paired via the OS,
  Pulp's MIDI backends consume it like any other MIDI device. See
  `core/midi/platform/` for the per-platform receivers.

### VST2 plugin format

- **Why:** Steinberg discontinued VST2 SDK licensing in 2018. Shipping
  VST2 support would require individual SDK licenses per developer,
  and the format is being actively deprecated from hosts.
- **What we DO support:** VST3 (open SDK), AU, CLAP (fully open), AAX
  (via the opt-in vendor carve-out in `CLAUDE.md`). LV2 read support
  is partial (see parity audit for details).

---

## Process for changing a non-goal

A non-goal only becomes an in-scope item when:

1. A concrete product use-case surfaces that Pulp's architecture can
   uniquely address (not something that works today with a separate
   library).
2. The maintainer chooses to update this file + the relevant VISION.md
   section in the same change.
3. An implementation spec lands in `planning/` with acceptance criteria
   and a build sequence.

Simply opening a GitHub issue saying "can we add X" is not enough to
move X off this list. The default answer is "not unless a planning
proposal justifies it."

---

## Related

- `VISION.md` — the positive scope statement
- `planning/reference-feature-gap-audit-2026-04-17.md` — parity audit
  that distinguished "missing but roadmap" from these deliberate
  non-goals
- `planning/cross-platform-parity-gap-audit-2026-04-16.md` — earlier
  parity audit
