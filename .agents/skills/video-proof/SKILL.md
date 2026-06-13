---
name: video-proof
description: Record, compose, publish, serve, and review short desktop validation video proofs for Pulp UX/test-harness work. Use when the user asks to "show me it working", record a validation video, prove a click/interaction, share a local proof link, attach a video to a review issue, or compare an implemented UI against source material. Covers macOS desktop capture, Remotion proof composition, attachment budgets, local/Tailscale serving, and review closure.
requires:
  scripts:
    - tools/local-ci/local_ci.py
    - tools/local-ci/scripts/compose-video-proof.mjs
  tools:
    - node
    - npm
---

# Validation video proofs

Use this skill when a screenshot is not enough to prove a UX interaction:
clicking a control, opening an inspector, checking a standalone/plugin host flow,
or showing an implementation next to source material. Default to still captures
for static assertions; record video only when motion, timing, audio/visual state,
or reviewer comprehension matters.

## Rules of engagement

- Work on a feature branch or dedicated worktree until humans have reviewed the
  proof workflow. Do not merge generated videos or local config.
- Prefer short clips: 5-12 seconds is the normal range. Keep the recording
  focused on the window or component under test.
- Prefer `video/proof-composed.mp4` for review. Keep `video/proof.mp4` as the
  raw debugging artifact.
- Treat size as a test result. Use `--video-attachment-budget-mb 100` for
  paid/pro GitHub issue attachment planning only when the uploader is eligible
  for videos above 10 MB. Otherwise use a 10 MB budget or publish a local report
  and link the served report instead of attaching the MP4.
- For review issues that may be handled by a non-pro uploader, rerender with
  `desktop compose-video --small-video --small-video-budget-mb 10` so
  `video/proof.small.mp4` is available as a convenience attachment.
- Never commit `tools/local-ci/config.json`, `tools/local-ci/node_modules/`,
  `.remotion/`, or generated desktop artifact bundles.

## First-time setup on a Mac

Start by printing the portable setup checklist. Use the machine label when you
are preparing another Mac, such as blackbook:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook
```

For a JSON handoff artifact that includes current readiness, add
`--check --run-in-terminal` so the readiness check uses Terminal.app's Screen
Recording grant:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook --check --run-in-terminal --json
```

Create the machine-local config on a new checkout:

```bash
cp tools/local-ci/config.example.json tools/local-ci/config.json
```

Install the repo-local developer ffmpeg and Remotion tooling:

```bash
npm --prefix tools/local-ci install
```

Smoke-test Remotion composition without requiring macOS Screen Recording:

```bash
npm --prefix tools/local-ci run smoke-video-proof
```

Enable optional video capture for the mac desktop target:

```bash
python3 tools/local-ci/local_ci.py desktop config set target.mac.video_capture true
```

Prepare the mac desktop target receipt:

```bash
python3 tools/local-ci/local_ci.py desktop install mac
```

Run the doctor before recording:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac
```

Required gates for video:

- `PASS screencapture`
- `PASS video_capture`
- `PASS target.video_capture`
- `PASS remotion_smoke`

`PASS avfoundation_screen` is preferred because it enables the primary
ffmpeg/AVFoundation recorder. If AVFoundation is hidden but `screencapture`
passes, the macOS recorder can still use its screencapture fallback.

Use `--skip-remotion-smoke` for a faster config/tooling check when you do not
need to rerender the synthetic Remotion smoke proof:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac --skip-remotion-smoke
```

If `avfoundation_screen` fails, ffmpeg cannot enumerate the primary macOS input
`Capture screen 0`; confirm the configured ffmpeg can run from the invoking
terminal and rerun the doctor. This is a warning, not a hard failure, when
`screencapture` passes.

If `screencapture` fails with `could not create image from display`, macOS has
not granted Screen Recording to the terminal or agent app. Ask the user to grant
Screen Recording, restart that app, then rerun the doctor. You can still update
docs/tests while waiting, but do not claim live capture has been validated.

When Terminal.app has Screen Recording permission but the current agent process
does not, run the same command through Terminal:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac --run-in-terminal
```

Use `--run-in-terminal` on `desktop video`, `desktop smoke --record-video`,
`desktop click --record-video`, or `desktop inspect --record-video` for the same
permission handoff. The wrapper removes the flag for the child command, relays
stdout/stderr and exit code, and sends a short `caffeinate -u` display-wake
pulse first so idle/remote displays do not produce black captures.

The macOS recorder uses ffmpeg/AVFoundation first and falls back to a
`screencapture -l` frame sequence when ffmpeg capture cannot start. If macOS
refuses window-ID capture but allows full-screen capture, the fallback captures
full-screen frames and crops them to the target window bounds during encoding.
Final still screenshots use a last-resort full-screen fallback for the same TCC
edge case. All paths still require Screen Recording permission for the invoking
app.

## Capture a proof

Record a short click proof and render the Remotion composition with the
dedicated video entry point:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --run-in-terminal \
  --command ./build/pulp \
  --click 120,80 \
  --video-note "Clicked the bypass control and verified the meter stayed visible." \
  --duration 8 \
  --video-fps 30 \
  --video-audio none \
  --video-attachment-budget-mb 100
```

Use `--action smoke`, `--action click`, or `--action inspect` to choose the
underlying desktop action. Today, recording is implemented for macOS only;
Linux/Windows video requests fail explicitly until their recorder backends are
wired.

Use repeatable `--video-note` flags for short proof points that should be
visible in the Remotion step list. This is especially useful for host/plugin
proofs where the video shows the host window and logs prove what was loaded.

For a console-style standalone app with no GUI window, record a titled
Terminal.app window instead of waiting for the child process to expose a
capturable window:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --run-in-terminal \
  --action smoke \
  --video-capture-target terminal \
  --command ./build/examples/pulp-tone/PulpTone.app/Contents/MacOS/PulpTone \
  --duration 5
```

Use terminal capture for smoke proofs only. It tees command output into the run
logs, captures the Terminal window, and records the command return code in the
manifest. Use normal app-window capture for clicks, ViewInspector snapshots, and
Pulp app automation.

Use named recipes when the user asks for one of the high-value demos:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe audio-inspector-demo \
  --command ./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo \
  --duration 4 \
  --video-fps 4
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe standalone-interaction \
  --command ./build/pulp \
  --click 120,80 \
  --duration 8
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe reaper-plugin-editor \
  --plugin PulpEffect \
  --plugin-format vst3 \
  --video-note "The wrapper opened the host and inserted the target plugin." \
  --click-view-id drive-knob \
  --duration 10
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe inspector-workflow \
  --command ./build/pulp \
  --duration 8
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe component-zoom \
  --command ./build/pulp \
  --component-id compressor-threshold \
  --duration 8
```

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --recipe design-parity \
  --command ./build/pulp \
  --source-image planning/screenshots/reference.png \
  --source-label "Figma reference" \
  --duration 8
```

The REAPER recipe defaults to bundle id `com.cockos.reaper` when no explicit
launch target is supplied, but it does not create a DAW project or insert the
plugin. Prepare the host session first, then use the recipe to capture and label
the proof consistently. If a wrapper command launches or prepares REAPER, add
`--capture-bundle-id com.cockos.reaper` so the harness records the REAPER window
rather than the wrapper process window. The `audio-inspector-demo` recipe is a smoke proof for a
built standalone demo binary; pass the command path for the build directory you
want to validate.

Recipes select Remotion templates and write structured setup context into
`video_proof_composition.context`. The composed video, `index.html`, and
`review.md` display that context, including recipe, host, plugin, format,
component, bundle id, and launch mode when available. `reaper-plugin-editor`
uses the `plugin-host` template, inspector recipes use `inspector-workflow`,
standalone interaction uses `standalone`, component proofs use
`component-zoom`, and source comparisons use `design-parity`.

The `component-zoom` recipe selects the Remotion `component-zoom` template. It
stores `video_proof_composition.focus` from the click selector and resolved
content point, so the composed proof shows a component label, focus rectangle,
and zoom detail inset. Use it when the reviewer should inspect one specific
control instead of hunting through the full app window.

The same recorder can be enabled on lower-level desktop actions:

```bash
python3 tools/local-ci/local_ci.py desktop click mac \
  --command ./build/pulp \
  --click 120,80 \
  --capture-before \
  --record-video \
  --compose-video-proof \
  --video-duration 8 \
  --video-fps 30 \
  --video-attachment-budget-mb 100
```

Adjust `--command`, `--click`, duration, and fps for the actual scenario. Keep
fps at 30 unless there is a motion-specific reason to go higher.

Keep `--video-audio none` unless the proof genuinely needs audio. Use
`--video-audio system --video-audio-device <index-or-name>` only when a known
macOS AVFoundation audio input or loopback device should be recorded; the same
device can be supplied with `PULP_VIDEO_AUDIO_DEVICE`. Do not guess a
microphone/system device. Audio-requested captures must fail rather than fall
back to a no-audio frame sequence. `--video-audio plugin` is still reserved
until plugin-origin audio capture exists.

Each run bundle should contain:

- `video/proof.mp4`
- `video/proof-composed.mp4` when `--compose-video-proof` is set
- `video/proof.issue.mp4` for GitHub/pro-account attachment review
- `video/proof.small.mp4` when `desktop compose-video --small-video` is used
- `video/metadata.json` with `size_bytes`, budget, and
  `fits_attachment_budget`
- `video/issue-metadata.json` with copy/transcode/budget status
- `video/small-metadata.json` with the 10 MB fallback status when requested
- screenshots, logs, and `manifest.json`

`proof.issue.mp4` is copied from the review source when it already fits the
configured budget. If the source is too large, the tool runs a bounded H.264
retry ladder: balanced 720p at 24 fps, compact 720p at 15 fps, then compact
540p at 15 fps. `issue-metadata.json` records every attempt and the final
`transcoded`, `exceeds-budget`, or `transcode-failed` status; use the served
report link when the issue variant still does not fit.

Published reports should contain both `review.md` and `review-package.json`.
Use `review.md` as the human GitHub issue body. Use `review-package.json` as the
structured source of truth for automation: it records whether to attach the
primary issue MP4, attach the small fallback, or use the served/local report
link, along with absolute attachment paths, size/budget fields, and fallback
serve commands.

To recompose an existing raw proof:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json
```

This rerenders `video/proof-composed.mp4`, refreshes composed metadata, creates
or refreshes `video/proof.issue.mp4`, writes issue metadata, and updates the run
manifest.

To create both the pro-account issue target and a 10 MB fallback:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json \
  --video-attachment-budget-mb 100 \
  --small-video \
  --small-video-budget-mb 10
```

For a design/source comparison proof, use the Remotion design-parity template:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json \
  --template design-parity \
  --source-image planning/screenshots/reference.png \
  --source-label "Figma reference" \
  --title "Design parity proof" \
  --note "The imported layout keeps the same primary control grouping."
```

This renders the source/reference image beside the captured proof and records a
`video_proof_composition` block in the manifest.

## Publish and serve for review

Publish the latest runs:

```bash
python3 tools/local-ci/local_ci.py desktop publish mac --limit 3 --label validation-video-proof
```

Publish a specific run or generated demo manifest when discovery is not enough:

```bash
python3 tools/local-ci/local_ci.py desktop publish \
  --manifest /path/to/run/manifest.json \
  --label validation-video-proof
```

Serve the latest published report on a Tailscale-visible machine:

```bash
python3 tools/local-ci/local_ci.py desktop serve --host 0.0.0.0 --port 8765
```

When bound to `0.0.0.0`, `desktop serve` prints candidate watch URLs:
localhost, the machine hostname, any comma-separated `PULP_DESKTOP_SERVE_HOSTS`,
and `tailscale ip -4` results when the Tailscale CLI is installed. If the
reviewer should use a friendly Tailnet DNS name, run with:

```bash
PULP_DESKTOP_SERVE_HOSTS=blackbook.tailnet-name.ts.net \
  python3 tools/local-ci/local_ci.py desktop serve --host 0.0.0.0 --port 8765
```

`desktop publish` writes `review.md` next to `index.html`. Use `review.md` as
the GitHub issue body. GitHub supports `.mp4`, `.mov`, and `.webm` attachments
and currently recommends H.264 for compatibility; paid-plan eligible uploaders
can use the 100 MB video budget, while others should assume 10 MB. Attach
`proof.issue.mp4` manually when it fits the chosen budget; attach
`proof.small.mp4` when the normal issue video is too large but the small fallback
fits. Otherwise include the served report URL. The review issue can be closed
when the reviewer comments `looks good to me`. The generated review body also
records the attach/do-not-attach decision and the `desktop verdict` commands for
approval or follow-up.

Record that review state back into the run manifest:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --approved \
  --issue-url https://github.com/owner/repo/issues/123
```

For another iteration, keep the issue open and capture the reason:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --needs-work \
  --notes "Zoom starts too late; recapture with the component centered."
```

`desktop verdict` writes a manifest `review` block with status, timestamp,
optional reviewer notes, and whether the review issue can be closed.

## High-value demo scenarios

- Standalone app: launch a built standalone, perform one visible interaction,
  and show the inspector or normal UI state changing.
- Plugin host: run a Pulp plugin in a host such as REAPER, click a meaningful
  control, and show host/plugin state updating.
- Audio Inspector: open the Audio Inspector during a running app and show live
  meters or probe state after a stimulus.
- Component zoom: focus on a specific implemented component and prove the
  interaction or visual state that was just changed.
- Design validation: show source material, the implemented UI, and a small
  comparison/diff frame so a reviewer can understand alignment without knowing
  the feature.

Use Remotion to make these clips reviewer-friendly: title card, scenario label,
run metadata, source identity, launch/action/capture timeline, size/budget
status, and short explanatory captions. Keep the composition factual and
concise; the video should explain the proof, not become a long demo reel.
