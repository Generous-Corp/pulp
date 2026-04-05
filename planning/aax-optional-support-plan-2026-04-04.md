# Optional AAX Support Plan

Date: 2026-04-04
Status: proposed
GitHub issue: #134 https://github.com/danielraffel/pulp/issues/134

## Purpose

Define a phased plan to add optional AAX Native support to Pulp alongside AU, AUv3, VST3, CLAP, and LV2 without bundling Avid SDK/tooling, violating the repo's license policy, or contaminating the codebase with non-Pulp source.

This is planning work only. It does not authorize implementation shortcuts around licensing, distribution, or clean-room rules.

## RepoPrompt Findings

RepoPrompt analysis of the current tree showed:

- Pulp already has a clean per-format adapter model centered on `Processor`, `PluginDescriptor`, `StateStore`, and per-format entry points.
- The likely extension points are `core/format/`, `tools/cmake/PulpUtils.cmake`, `tools/cmake/PulpPlugin.cmake`, `tools/cli/pulp_cli.cpp`, `test/`, and the format docs/status surfaces.
- The architecture can support AAX without changing the core processor API if phase 1 is limited to AAX Native effect/instrument support.
- The real risk is policy and packaging drift, not the adapter shape itself.
- Current repo policy is internally inconsistent for AAX:
  - `DEPENDENCIES.md` currently says proprietary dependencies are not allowed.
  - The same file also lists the AAX SDK as a planned developer-obtained dependency.
  - `docs/reference/licensing.md` currently claims every dependency is MIT-compatible.
- Any AAX work must therefore start by making the optional proprietary carve-out explicit and enforceable.

## Non-Negotiable Guardrails

These rules should be treated as hard constraints for any future AAX work:

- AAX remains optional and off by default.
- The AAX SDK is developer-provided and out-of-tree.
- No `FetchContent`, no bootstrap clone, and no automatic AAX download flow.
- No Avid SDK files, headers, libraries, example projects, validators, page table tools, or Pro Tools assets are committed to this repo.
- No Avid tooling is exported as part of `cmake --install` or the installed Pulp SDK.
- Public CI must not require Avid or PACE tooling.
- Public Pulp releases must not claim AAX packaging/signing support until the vendor-required workflow is validated.
- We do not use JUCE's AAX wrapper as implementation reference.
- We do not copy Avid example implementation code into the repo. If AAX sample projects are consulted to confirm bundle layout or exported symbols, the resulting implementation must be original Pulp code and limited to the minimum ABI facts required for compatibility.

## Licensing Position

The local AAX SDK package includes `LICENSE.txt` stating the SDK is subject to commercial or open-source licensing, with a GPLv3 path also mentioned. That has two immediate implications for Pulp:

- Pulp cannot treat AAX as a normal MIT-compatible dependency.
- Pulp should not build its public AAX story around the GPL option, because the repo policy explicitly rejects copyleft dependencies.

The safe position for this project is:

- Pulp stays MIT.
- AAX support is an optional integration path that requires a separately obtained Avid license and separately downloaded Avid SDK/tooling.
- Users are responsible for complying with Avid's terms for their own AAX development and distribution workflows.
- Pulp does not redistribute Avid material and does not grant AAX rights by virtue of its MIT license.

## Current Avid Download Guidance

From the current Avid surfaces and the local SDK/tool readmes:

- A developer needs an Avid account to access the AAX SDK/toolkit downloads.
- Avid routes the AAX SDK through a click-through license flow.
- Avid states that commercializing AAX products requires additional tools/license steps and directs developers to contact `audiosdk@avid.com`.
- Avid states an iLok account is required for Pro Tools AAX testing.
- The local SDK README also notes that Pro Tools requires digitally signed AAX plug-ins and that special developer builds of Pro Tools can run unsigned plug-ins.
- The validator and DigiShell packages can exercise AAX bundles directly, which means we can do meaningful early validation without relying on a full Pro Tools install.

That gives us a practical split:

- Early development: build, load, and validate using AAX SDK + DigiShell/AAX Validator.
- Later smoke testing: Pro Tools developer build or standard Pro Tools workflow once signing/authorizations are available.
- Commercial release path: separate Avid/PACE process, explicitly outside phase 1.

## Local Avid Artifact Retention Decision

Keep these files in `/Users/danielraffel/Desktop/avid` for planned AAX integration work:

- `aax-sdk-2-9-0.zip`
- `aax-validator-dsh-2024-6-0-138bab0d-mac-arm64.tar.gz`
- `aax-validator-dsh-2024-6-0-138bab0d-mac-x86_64.tar.gz`
- `aax-page-table-editor-2024-6-0-138bab0d-mac-arm64.tar.gz`
- `AAX_Plug-In_Burnthrough_Grid_2024Jan30.pdf`

Rationale:

- `aax-sdk-2-9-0.zip` is the required SDK payload.
- `aax-validator-dsh-*` provides the native validation and DigiShell hosting path needed before Pro Tools is introduced.
- Keeping both macOS validator architectures preserves an Intel validation option later without needing an immediate re-download.
- The arm64 page table editor is useful later for page-table generation/editing and is small enough to justify keeping.
- The burnthrough grid PDF is useful as a local-only manual validation reference and must not be copied into the repo.

Delete these files because they are unnecessary for the planned native-AAX integration path, obsolete relative to newer packages, or clean-room risks:

- `CloudClientServices_22.10.0_Mac.dmg`
- `CloudClientServices_22.10.0_Win.zip`
- `HD_Driver_24.10.0_Mac.dmg`
- `HD_Driver_24.10.0_Win.zip`
- `Low_Roar_Pro_Tools_Demo.zip`
- `Pro Tools/`
- `ProToolsKTrace_v13.tar.gz`
- `ProToolsWPRTool_0.9.zip`
- `Pro_Tools_25.12.0_Mac.dmg`
- `aax-developer-tools-mac-arm64-beta-2022-R4-01-20220930.tar.gz`
- `aax-developer-tools-mac-x86_64-beta-2022-09-01.tar.gz`
- `aax-page-table-editor-2024-6-0-138bab0d-mac-x86_64.tar.gz`
- `aax-validator-dsh-2024-6-0-dc68c2dd-win-x86_64.zip`
- `juce_to_aax_dsp_5p4p1_20191011.zip`

Important note:

- The JUCE example zip should not remain in the active AAX work folder because it includes JUCE AAX wrapper source and creates an avoidable clean-room hazard for this repo.

## Proposed Scope

Initial implementation scope should be intentionally narrow:

- AAX Native only
- macOS first
- effect and instrument categories only
- no AudioSuite
- no DSP or hybrid targets
- no custom AAX GUI in the first delivery
- no public release packaging/signing automation in phase 1

This keeps the work aligned with the current `Processor` model and avoids overcommitting to AAX-specific surfaces before the native path is proven.

## Session Execution Checklist

If this work is driven through a single implementation session, the work should still happen in this order:

1. Tighten policy/docs so the repo explicitly distinguishes MIT-shipped code from optional developer-supplied vendor SDKs.
2. Add hard guardrails that fail when Avid SDK files, AAX example code, validator bundles, or other vendor artifacts are committed to the repo.
3. Add `PULP_ENABLE_AAX` and `PULP_AAX_SDK_DIR` with out-of-tree SDK validation and clear failure messages.
4. Build an original Pulp-owned AAX metadata layer that maps `PluginDescriptor` and `StateStore` into AAX-facing identifiers, categories, parameters, and chunk/state behavior.
5. Add an AAX Native processing path that wraps `Processor` without changing the core processor contract.
6. Add AAX entry/registration scaffolding and plugin target generation in `pulp_add_plugin(...)`.
7. Keep all AAX glue per-plugin and Pulp-owned; never install or export Avid headers, sources, static libraries, validators, or other tools.
8. Add AAX-specific unit tests and bundle-load validation that work without Pro Tools and prefer AAX Validator/DigiShell when locally available.
9. Extend CLI surfaces (`status`, `doctor`, `create`, `validate`, `ship`) so AAX is reported honestly as optional, developer-supplied, and not publicly packaged/signed by default.
10. Add templates, examples, and installed-SDK support files needed for downstream consumers to opt into AAX with their own SDK copy.
11. Finish with onboarding docs that explain Avid account setup, SDK download, validator-first testing, Pro Tools developer-build caveats, and the commercial signing boundary.

## Phase Plan

### Phase 0: Policy and dependency carve-out

Goal:

- Make optional proprietary AAX support explicit, truthful, and enforceable in repo policy.

Work:

- Update `DEPENDENCIES.md` to distinguish bundled MIT-compatible dependencies from optional developer-supplied proprietary SDKs.
- Update `docs/reference/licensing.md` so it no longer claims every dependency in the total ecosystem is MIT-compatible.
- Add an AAX entry to `tools/deps/manifest.json` as optional, developer-supplied, and not redistributed.
- Define the rule that `PULP_AAX_SDK_DIR` must point outside the source tree.

Exit criteria:

- Repo docs tell the truth about AAX.
- The policy makes clear that AAX is not bundled and not covered by the repo's normal dependency bootstrap path.

### Phase 1: AAX adapter skeleton and metadata validation

Goal:

- Prove that Pulp can express AAX Native without changing the core processor contract.

Work:

- Add AAX entry and adapter scaffolding under `core/format/`.
- Add a metadata builder that converts `PluginDescriptor` and parameter info into a frozen AAX-facing description.
- Validate unsupported cases early:
  - `MidiEffect`
  - unsupported bus shapes
  - unsupported channel layouts
  - unsupported multi-bus combinations
- Add AAX-specific unit tests for entry/metadata behavior.

Exit criteria:

- AAX metadata and entry tests pass when the AAX SDK is present.
- The adapter layer compiles without changing existing AU/VST3/CLAP/LV2 behavior.

### Phase 2: Build-system integration

Goal:

- Build an AAX bundle from in-tree Pulp examples without exporting or bundling Avid code.

Work:

- Introduce `PULP_ENABLE_AAX`.
- Introduce `PULP_AAX_SDK_DIR`.
- Add a dedicated internal CMake helper for AAX SDK discovery and validation.
- Extend `pulp_add_plugin(...)` to accept `AAX`.
- Enforce out-of-tree SDK paths and fail clearly if users try to point at a repo-local copy.
- Keep AAX glue compiled per-plugin, not folded into a public redistributable target.

Exit criteria:

- AAX build is opt-in.
- Pulp can emit an `.aaxplugin` bundle on supported hosts.
- `cmake --install` does not export vendor SDK material.

### Phase 3: Validation without Pro Tools

Goal:

- Establish a serious validation loop before introducing Pro Tools.

Work:

- Add a bundle-load test that verifies the AAX bundle exports the expected entry points.
- Add a CLI validation path that prefers AAX Validator when available and falls back to a lightweight load check.
- Wire DigiShell/AAX Validator usage into docs and developer workflow.
- Keep Pro Tools out of the critical path for early bring-up.

Exit criteria:

- Developers can build and validate AAX bundles locally using only the SDK and validator tools.
- Validation failures are readable and actionable.

### Phase 4: CLI, scaffolding, and installed-SDK support

Goal:

- Make AAX a first-class optional format in the Pulp developer workflow.

Work:

- Extend `pulp status`, `doctor`, `validate`, and `ship` to report AAX honestly.
- Add `pulp create --formats ...` so AAX can be explicitly selected rather than defaulted.
- Add `aax_entry.cpp` templates for effect/instrument plugin scaffolds.
- Install only Pulp-owned AAX support files for `find_package(Pulp)` consumers; never install Avid SDK content.

Exit criteria:

- AAX is selectable in normal Pulp project generation.
- Installed SDK consumers can build AAX if they separately provide `PULP_AAX_SDK_DIR`.

### Phase 5: Docs and onboarding

Goal:

- Tell users exactly how to obtain the SDK and what Pulp does and does not provide.

Work:

- Add AAX onboarding docs covering:
  - Avid Master Account creation
  - AAX SDK click-through download flow
  - iLok/PACE prerequisites
  - local SDK/tool placement outside the repo
  - `PULP_AAX_SDK_DIR`
  - validation without Pro Tools
  - what remains unsupported in phase 1
- Update format/build/testing/licensing/support-matrix docs.

Exit criteria:

- A new developer can set up local AAX work without guessing.
- Docs clearly separate local development, validator-driven validation, Pro Tools smoke testing, and commercial release requirements.

### Phase 6: Private validation and release path

Goal:

- Add the commercial/distribution workflow only after the engineering path is stable.

Work:

- Add private or self-hosted validation where Avid/PACE tooling is legally and operationally available.
- Add Pro Tools smoke testing where appropriate.
- Add AAX-specific packaging/signing/wrapping steps only when the vendor-required workflow is in place.

Exit criteria:

- AAX is not marked more mature than the actual signing/validation pipeline supports.
- Public CI remains clean and legally safe.

## Documentation Work That Should Exist Before Claiming Support

Minimum docs set:

- `docs/guides/formats.md`
- `docs/guides/build.md`
- `docs/guides/testing.md`
- `docs/guides/testing-advanced.md`
- `docs/guides/shipping.md`
- `docs/guides/troubleshooting.md`
- `docs/guides/platforms/macos.md`
- `docs/reference/cmake.md`
- `docs/reference/cli.md`
- `docs/reference/licensing.md`
- `docs/reference/capabilities.md`
- `docs/status/support-matrix.yaml`

Suggested AAX onboarding content:

1. Create or sign in to an Avid Master Account.
2. Accept the AAX SDK click-through agreement from the Avid developer portal.
3. Create an iLok account.
4. Install PACE/iLok tooling if validator or signed-host workflows require it.
5. Download the AAX SDK and AAX Validator packages.
6. Keep them outside the Pulp repo.
7. Configure `PULP_AAX_SDK_DIR`.
8. Build and validate with the local validator before attempting Pro Tools smoke tests.
9. Treat Pro Tools and commercial/PACE signing as later-stage work, not the initial bring-up path.

## Open Questions To Resolve During Implementation

- Which exact AAX SDK files should be used as CMake path sentinels.
- Which exported symbols and resource layout are required for the pinned SDK version.
- Whether the native macOS validator is sufficient for all macOS slice validation or whether a stricter Intel validation lane is needed.
- Which PACE/iLok prerequisites are required for validator-only workflows versus Pro Tools workflows.
- How much page-table support is required for an acceptable phase 1 validator result.
- Whether Windows AAX should stay `planned` until a separate host/tooling lane exists.

## Recommendation

Proceed with issue #134 as a phased future-feature track.

Do not start by writing the adapter.
Start by fixing the policy story and the build-system boundaries so that when AAX code lands, it lands behind the correct legal and packaging constraints from day one.
