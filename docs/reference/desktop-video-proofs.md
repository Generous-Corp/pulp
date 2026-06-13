# Desktop Video Proofs

Desktop automation can optionally record a short MP4 proof for macOS local
targets. Video is for UX interactions where a still screenshot or before/after
diff does not explain enough.

## Requirements

- The desktop target should enable optional `video_capture`.
- The runner process must have macOS Screen Recording permission.
- The runner needs ffmpeg, either from `PULP_FFMPEG`, `PATH`, or the
  repo-local developer install.
- `pulp ci-local desktop doctor mac` reports Screen Recording, ffmpeg, and
  AVFoundation screen-input state.

Print the portable first-run checklist for another Mac, such as blackbook:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook
```

Use `--json` for a handoff artifact and `--check --run-in-terminal` to include
the current `video-doctor` readiness payload via Terminal.app's Screen Recording
grant:

```bash
python3 tools/local-ci/local_ci.py desktop video-setup mac --machine blackbook --check --run-in-terminal --json
```

Create the machine-local config on a new checkout:

```bash
cp tools/local-ci/config.example.json tools/local-ci/config.json
```

Install the repo-local developer ffmpeg on a new Mac:

```bash
npm --prefix tools/local-ci install
```

Verify the Remotion/ffmpeg composition stack without Screen Recording
permission:

```bash
npm --prefix tools/local-ci run smoke-video-proof
```

Enable video capture for the mac target:

```bash
python3 tools/local-ci/local_ci.py desktop config set target.mac.video_capture true
```

Prepare the mac desktop target receipt:

```bash
python3 tools/local-ci/local_ci.py desktop install mac
```

Run the video-specific readiness gate:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac
```

For a faster config/tooling check that skips the Remotion smoke render:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac --skip-remotion-smoke
```

`video-doctor` should show `PASS screencapture`, `PASS video_capture`,
`PASS target.video_capture`, and `PASS remotion_smoke` before `--record-video`
can produce a composed clip. `PASS avfoundation_screen` is preferred because it
uses the primary ffmpeg/AVFoundation path; if it fails while `screencapture`
passes, the recorder can still use the screencapture fallback. If
`screencapture` reports
`could not create image from display`, grant Screen Recording permission to the
terminal/agent app that runs local CI, then restart that app.

When Terminal.app has Screen Recording permission but the current agent process
does not, run the readiness check through Terminal:

```bash
python3 tools/local-ci/local_ci.py desktop video-doctor mac --run-in-terminal
```

`--run-in-terminal` re-invokes the same local-ci command inside Terminal.app,
removes the flag for the child process, and relays the child stdout/stderr and
exit code. It also sends a short `caffeinate -u` display-wake pulse before the
child command so idle/remote displays do not produce black captures.

## Capture

The dedicated entry point records video by default and renders the Remotion
composition unless `--compose-video-proof` is already set explicitly:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --run-in-terminal \
  --command ./build/pulp \
  --click 120,80 \
  --duration 8 \
  --video-fps 30 \
  --video-audio none \
  --video-attachment-budget-mb 100
```

Use `--action smoke`, `--action click`, or `--action inspect` to choose the
underlying desktop action. The command currently records video on macOS only;
Linux and Windows fail explicitly rather than producing a run bundle without a
video artifact. Use `--run-in-terminal` on macOS when Terminal is the process
with Screen Recording permission.

For console-style standalone apps that do not create their own GUI window, ask
the harness to record a titled Terminal.app window instead of waiting for the
child process to expose a window:

```bash
python3 tools/local-ci/local_ci.py desktop video mac \
  --run-in-terminal \
  --action smoke \
  --video-capture-target terminal \
  --command ./build/examples/pulp-tone/PulpTone.app/Contents/MacOS/PulpTone \
  --duration 5
```

Terminal capture is for smoke proofs only. It tees the command output into the
normal run logs, keeps the Terminal window open for the capture duration, then
signals the child process and records the command return code in the manifest.

Named recipes apply reviewer-friendly defaults for common proof scenarios:

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
  --click-view-id drive-knob \
  --duration 10
```

The REAPER recipe defaults to launching bundle id `com.cockos.reaper` when no
explicit `--command` or `--bundle-id` is supplied. It still needs a prepared host
session/project that opens the target plugin editor; the recipe supplies proof
metadata, labels, and composition title defaults, not DAW project automation.

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

`component-zoom` enables ViewInspector capture and before/diff capture, then
uses `--component-id` as the click target when no explicit click selector was
provided. `audio-inspector-demo` records a smoke proof of a built standalone
Audio Inspector demo window without requiring a UI snapshot artifact.
`design-parity` records an inspect proof and composes it with the Remotion
`design-parity` template directly during capture.

The same recorder can be enabled on lower-level desktop actions:

```bash
./tools/local-ci/local_ci.py desktop click mac \
  --command ./build/pulp \
  --click 120,80 \
  --capture-before \
  --record-video \
  --compose-video-proof \
  --video-duration 8 \
  --video-fps 30 \
  --video-attachment-budget-mb 100
```

The run bundle records:

- `video/proof.mp4`
- `video/proof-composed.mp4` when `--compose-video-proof` is set
- `video/proof.issue.mp4` as the configured GitHub/pro-account attachment target
- `video/proof.small.mp4` when `desktop compose-video --small-video` is used
- `video/metadata.json`
- `video/issue-metadata.json`
- `video/small-metadata.json` when the small fallback is rendered
- screenshot, before/diff screenshot, logs, and `manifest.json`

`metadata.json` includes `size_bytes`, the attachment budget, and
`fits_attachment_budget`. Keep issue-ready clips under the configured budget.
For GitHub issue review, use 100 MB only for paid-plan video uploaders who are
eligible to upload videos above 10 MB; otherwise use 10 MB or fall back to a
local/internal link. GitHub supports `.mp4`, `.mov`, and `.webm` attachments and
recommends H.264 for compatibility.
When the review source is already under budget, `proof.issue.mp4` is a copy of
that source. When it is over budget, the tool runs a bounded H.264 retry ladder:
balanced 720p at 24 fps, compact 720p at 15 fps, then compact 540p at 15 fps.
`issue-metadata.json` records each attempt, the selected attempt when one fits,
and final `status=transcoded`, `exceeds-budget`, or `transcode-failed`.
For review lanes that should always prepare a 10 MB fallback, rerender with
`--small-video`; this writes `proof.small.mp4` and `small-metadata.json` using
`--small-video-budget-mb` independently from the pro-account issue budget.

`--video-audio none` is the only implemented audio mode in the MVP. Reserved
values `plugin` and `system` fail fast so the tool never silently records
microphone or system audio. Remotion composition is rendered muted until an
explicit audio source lands, so `proof-composed.mp4` and `proof.issue.mp4` do
not imply audio validation.

## Remotion Composition

`--compose-video-proof` uses the repo-local Remotion tooling to render an
annotated proof video with run context, source identity, a launch/action/capture
timeline, attachment status, and the raw recording. The composed clip is what
reviewers should watch first.

You can rerender a composed clip from an existing run bundle:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json
```

That command writes `video/proof-composed.mp4`, `video/composed-metadata.json`,
`video/proof.issue.mp4`, and `video/issue-metadata.json`, then updates the run
manifest. The lower-level npm script is still available for template iteration:

For source/design comparison reviews, pass a reference image and the
`design-parity` template:

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json \
  --template design-parity \
  --source-image planning/screenshots/reference.png \
  --source-label "Figma reference" \
  --title "Design parity proof" \
  --video-attachment-budget-mb 100 \
  --small-video \
  --small-video-budget-mb 10
```

That composition renders the source/reference image beside the captured proof
and records a `video_proof_composition` block in the run manifest.

```bash
npm --prefix tools/local-ci run compose-video-proof -- \
  --manifest /path/to/run/manifest.json \
  --output /path/to/run/video/proof-composed.mp4
```

## Local Review

Stage a local report after one or more desktop runs:

```bash
./tools/local-ci/local_ci.py desktop publish mac --limit 3 --label validation-video-proof
```

To stage a specific run or generated demo manifest, pass it explicitly:

```bash
python3 tools/local-ci/local_ci.py desktop publish \
  --manifest /path/to/run/manifest.json \
  --label validation-video-proof
```

The generated `index.html` renders videos with native browser controls. To
share from a Tailscale-visible machine, serve the publish directory with a local
static server and link that URL in the review issue:

```bash
python3 tools/local-ci/local_ci.py desktop serve --host 0.0.0.0 --port 8765
```

`desktop serve` serves the latest published desktop report unless a report
directory is passed explicitly. When bound to `0.0.0.0`, it prints candidate
watch URLs: localhost, this machine's hostname, any comma-separated
`PULP_DESKTOP_SERVE_HOSTS`, and `tailscale ip -4` results when the Tailscale CLI
is available. Set `PULP_DESKTOP_SERVE_HOSTS=blackbook.tailnet-name.ts.net` when
you already know the friendly Tailnet DNS name you want reviewers to tap.

`desktop publish` also writes `review.md` next to `index.html`. Use that file
as the GitHub issue body. It includes the local report path, a Tailscale/local
serve command, each run's video artifact, attachment-budget status, and the
expected reviewer response. When a small fallback exists, it also lists
`proof.small.mp4`, its 10 MB budget status, and whether that smaller file should
be attached instead of the pro-account issue video. It includes the concrete
attach/do-not-attach decision and `desktop verdict` commands for approval or
follow-up. GitHub's
current hosted attachment policy is 100 MB for paid-plan video uploads when the
uploader is eligible; otherwise plan for the 10 MB video cap and use the served
report link. The intended review loop is:

1. Publish the report.
2. Open a GitHub issue with the generated `review.md` body.
3. Attach the MP4 manually when it fits the configured budget, or use the served
   report link when it does not.
4. Record the review verdict in the run manifest once the reviewer responds.
5. Close the issue once the reviewer comments `looks good to me`.

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --approved \
  --issue-url https://github.com/owner/repo/issues/123
```

If the proof needs another iteration, record that state instead:

```bash
python3 tools/local-ci/local_ci.py desktop verdict /path/to/run/manifest.json \
  --needs-work \
  --notes "Zoom starts too late; recapture with the component centered."
```

The manifest `review` block records `approved` or `needs-work`, the review
timestamp, optional reviewer notes, and whether the review issue can be closed.

## Current Scope

This first lane records the target window region on macOS with H.264 video and
no audio. It uses ffmpeg/AVFoundation screen capture as the primary recorder
and falls back to a short sequence of trusted `screencapture -l` window frames
when the ffmpeg recorder cannot start. If macOS refuses window-ID capture but
allows full-screen capture, the fallback captures full-screen frames and crops
them to the target window bounds during encoding. Final still screenshots use a
last-resort full-screen fallback for the same TCC edge case.

Remotion composition and local review artifacts are implemented in this branch.
Audio capture, iOS Simulator, Android, full REAPER project/plugin automation,
and GitHub issue automation are planned follow-on layers.
