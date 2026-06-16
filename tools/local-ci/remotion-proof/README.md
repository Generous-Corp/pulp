# Pulp video proof composition

This is developer-only tooling for turning desktop automation run bundles into
short annotated proof videos. It is not shipped with Pulp and is not required by
public CI.

## License boundary

The committed files in this directory are Pulp-owned source. They depend on
Remotion packages installed from npm by the developer. Remotion is
source-available with its own licensing terms; companies over Remotion's free
usage threshold need an appropriate Remotion license to run this composer.

`node_modules/`, generated videos, and Remotion caches must stay out of git.

## First-time setup

From the repository root:

```bash
npm --prefix tools/local-ci install
```

That installs the pinned Remotion packages and `ffmpeg-static`. The runtime
ffmpeg resolver also accepts `PULP_FFMPEG`, `PULP_FFMPEG_PATH`, or a system
`ffmpeg` on `PATH`.

## Smoke render

This does not need macOS Screen Recording permission. It creates a synthetic raw
MP4 with ffmpeg-static, composes it with Remotion, and writes ignored outputs
under `tools/local-ci/.video-proof-smoke/`:

```bash
npm --prefix tools/local-ci run smoke-video-proof
```

Expected output:

- `tools/local-ci/.video-proof-smoke/raw-proof.mp4`
- `tools/local-ci/.video-proof-smoke/poster.png`
- `tools/local-ci/.video-proof-smoke/manifest.json`
- `tools/local-ci/.video-proof-smoke/proof-composed.mp4`

## Compose a real run bundle

```bash
python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json
```

That command updates the run manifest with `video_composed` and `video_issue`
metadata. For direct template iteration, run the lower-level composer:

```bash
npm --prefix tools/local-ci run compose-video-proof -- \
  --manifest /path/to/run/manifest.json \
  --output /path/to/run/video/proof-composed.mp4
```

The composer reads the run manifest, copies the raw video/poster into a temp
Remotion public directory, renders `ValidationProof`, and writes a compact H.264
MP4. The proof includes a title card, raw capture, source identity, a
launch/action/capture/review timeline, attachment-size status, and concise notes
from the run metadata. Composition stays muted unless the run metadata explicitly
records `has_audio=true` with a non-`none` audio source; system-audio proofs pass
that audio through as AAC.

Screen Recording permission is only required for real desktop capture, not for
the Remotion smoke render.
