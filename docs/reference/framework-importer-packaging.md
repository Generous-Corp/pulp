# Framework-importer packaging

Pulp can import projects from third-party plugin frameworks through **importer
add-ons**: vendor-specific tools that speak Pulp's JSON-over-stdio import SPI
(`tools/import/schemas/import-spi-v0.schema.json`). The SDK ships the *consumer*
of these add-ons — `pulp tool install <importer>` / `pulp add <importer>` — but
the add-ons themselves are produced and distributed separately.

This page has two parts:

1. **The install-side contract** (built + shipping today) — what an importer
   package must look like so `pulp tool install` can verify and install it.
2. **A decision memo** (NOT yet decided) — the producer-side questions that need
   a maintainer's call before any importer ships as a real binary: how artifacts
   are built/hosted/pinned, the bundled-libclang license tradeoff, and signing /
   notarization. The CLI consumes whatever these decisions land on; it does not
   make them.

---

## 1. The install-side contract (shipping)

An importer is a tool-registry entry (`tools/packages/tool-registry.json`) with
`category: "importer"`. The SDK code names no vendor — framework identity is
runtime DATA carried by the descriptor.

```jsonc
{
  "framework-x-importer": {
    "display_name": "Framework X Importer",
    "category": "importer",
    "license": "Apache-2.0",
    "install_method": "importer_package",
    "pinned_version": "1.4.2",

    // Discovery + capability DATA (vendor-named here, never in SDK code).
    "frameworks": ["framework-x"],
    "capabilities": ["detect", "inspect", "emit"],

    // Version negotiation windows.
    "spi_min": 0, "spi_max": 0,        // import-SPI versions this importer speaks
    "sdk_min": "0.380.0",              // lowest Pulp SDK it supports
    "sdk_max": "0.400.0",             // highest Pulp SDK it supports

    // The skill that ships with the importer.
    "skill_source": "SKILL.md",        // path inside the archive
    "skill_name": "framework-x-importer",  // ~/.agents/skills/<this>/ (defaults to id)

    // Accept-gate metadata (composes with the importer-terms gate).
    "terms_version": "2026-06-07",
    "vendor_id": "framework-x",

    // Checksummed, per-platform artifacts.
    "importer_artifacts": {
      "macOS-arm64": {
        "url_template": "https://example.org/framework-x-importer-${version}-macos-arm64.tar.gz",
        "archive_format": "tar.gz",
        "sha256": "…64 hex chars…"
      },
      "Linux-x64":  { "...": "..." },
      "Windows-x64": { "...": "..." }
    }
  }
}
```

`pulp tool install <importer>` enforces three gates, in order:

1. **Version window.** The running SDK must fall within `[sdk_min, sdk_max]`
   (inclusive when set) and the importer's `[spi_min, spi_max]` must overlap the
   SDK's supported import-SPI window. A mismatch fails **loudly** with an
   actionable message (`upgrade Pulp` / `upgrade the importer`) and installs
   nothing.
2. **Checksum.** The fetched (or `--from`-supplied) archive's SHA-256 must equal
   the `sha256` pinned in the registry. A mismatch **refuses** to install.
3. **Skill + record.** On success the importer's `SKILL.md` is written to
   `~/.agents/skills/<skill_name>/SKILL.md` and the install is recorded under
   `~/.pulp/importers/<id>.json` (id, version, sha256, SDK version, SPI window,
   install dir, skill path, terms metadata). `pulp tool uninstall <importer>`
   reverses all three.

`--from <path|file://...>` installs from a local package instead of the registry
URL — for offline installs, pinned/air-gapped artifacts, and tests. The same
checksum + version-window gates still apply.

**Composition with the importer-terms gate.** Install and *run* are separate.
The accept-to-run IMPORTER_TERMS gate applies when an importer is first *run*
(`pulp import …`); install only fetches/verifies the package and records it. The
install record under `~/.pulp/importers/` carries `terms_version` /
`terms_vendor_id` so the terms gate can key acceptance to the installed version.

---

## 2. Decision memo — NEEDS A MAINTAINER DECISION

The contract above defines what the CLI *consumes*. The following producer-side
questions are deliberately **not decided here**. Each needs a maintainer call and
a release-pipeline change before any importer ships as a real prebuilt binary.

### 2a. Artifact production / hosting / pinning

**Question:** How are per-platform importer artifacts built, where are they
hosted, and how is each pinned to an SDK release?

Considerations:

- **Build matrix.** Each importer needs `macOS-arm64`, `macOS-x64` (or a
  universal binary), `Linux-x64`, `Linux-arm64`, and `Windows-x64`. The importer
  links libclang + the shared extractor substrate; producing these is a CI job in
  the *add-on* repo, not the public Pulp repo.
- **Hosting.** GitHub Releases on the private add-on repo, an object store
  (S3/R2/GCS), or a Pulp-run CDN. Whatever is chosen must serve a stable
  `${version}`-templated URL and a side-channel for the SHA-256 that goes into
  the registry.
- **Pinning per SDK release.** Because the install gate enforces
  `[sdk_min, sdk_max]`, every SDK release that changes the import-SPI or the
  emitted-scaffold contract needs a matching importer build with a widened/shifted
  window, and a registry entry update. Decide whether importer versions track SDK
  versions 1:1 or float with their own cadence inside a supported window.
- **SPI window width.** A narrow `[spi_min, spi_max]` (often degenerate `[v, v]`
  today) is safest but forces a new importer per SPI bump; a wider window reduces
  churn but obligates the importer to handle multiple SPI versions. Pick a policy.

**Recommendation to evaluate (not decided):** per-add-on-repo CI build → GitHub
Releases on the private repo → registry entry carries the release URL + sha256;
importer version floats within a small SPI window, re-pinned when the SPI bumps.

### 2b. Bundled libclang vs. system libclang

**Question:** Should each importer bundle a fixed libclang, or require a
system-installed one?

| | Bundled libclang (fixed LLVM release) | System libclang |
|---|---|---|
| Reproducibility | Deterministic AST across machines | Varies by host LLVM version |
| Clean-machine UX | Works with zero extra installs | Requires the user to install LLVM |
| Artifact size | Large (tens–hundreds of MB per platform) | Small |
| Maintenance | Pin + periodically bump one LLVM | None, but support burden from version drift |
| License obligation | Must preserve the LLVM notices (below) | None added by Pulp |

**License note (load-bearing).** LLVM/Clang (incl. libclang) is licensed under
**Apache-2.0 WITH LLVM-exception**, which is on Pulp's allowed list. Bundling it
in a redistributed artifact obligates the producer to **preserve the LLVM
`LICENSE.TXT` / NOTICE** alongside the binary and to add an entry to the add-on's
`DEPENDENCIES.md` / `NOTICE.md`. This is an attribution obligation, not a
copyleft one — there is no source-disclosure requirement. The plan's stated
default ("bundled libclang, Apache-2.0-with-LLVM-exception, notices preserved")
is license-clean; the open decision is the **size / maintenance** tradeoff, not
legality.

**Recommendation to evaluate (not decided):** bundle a pinned libclang for the
clean-machine acceptance criterion in the plan ("a clean machine can install an
importer and run `pulp import` without a system libclang"), accept the artifact
size, and carry the LLVM notice in the add-on package + its NOTICE.md.

### 2c. Signing / notarization of artifacts

**Question:** How are importer binaries signed so they run without OS friction?

Considerations:

- **macOS.** An unsigned/un-notarized importer binary trips Gatekeeper
  (`com.apple.quarantine`). The install path already strips quarantine on the
  *unpacked* tree, but a downloaded executable should be **Developer-ID signed +
  notarized + stapled** so it runs cleanly off a fresh download. Decide whose
  Developer ID signs add-on artifacts (Pulp's, or each add-on vendor's).
- **Windows.** Authenticode signing (signtool) to avoid SmartScreen warnings.
- **Linux.** No OS signing requirement; rely on the SHA-256 gate for integrity.
  Optionally publish a detached signature (minisign/cosign) for supply-chain
  verification.
- **Integrity vs. authenticity.** The registry SHA-256 gate gives *integrity*
  (the bytes match what the registry pins) but not *authenticity* (who produced
  them). Decide whether to add a signature-verification step to the install path
  (e.g. cosign over the artifact) on top of the checksum, and where the trust
  root lives.

**Recommendation to evaluate (not decided):** macOS Developer-ID +
notarize/staple, Windows Authenticode, Linux SHA-256 (+ optional cosign);
consider adding a cosign/minisign verification step to the install gate in a
later slice once a signing identity and trust root are chosen.

---

## Status

- **Install mechanism (this page, §1):** built, tested
  (`test/test_cli_importer_install.cpp`), shipping.
- **Producer side (§2a–2c):** **pending maintainer decisions** + a release
  pipeline. No real importer artifacts are produced or hosted yet.
