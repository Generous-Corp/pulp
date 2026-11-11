#!/usr/bin/env python3
"""Generate the audio-decoder fuzz seed corpus.

Usage:
    python3 test/fuzz/audio_decoder_seed_corpus.py <out_dir>

Writes one plain container file per seed into <out_dir> (flat, no tag
prefix), named `<format>-<description>.<ext>`. WAV, AIFF, RF64, W64 and the
hand-built FLAC seeds are constructed byte by byte with the Python standard
library and are reproducible by construction. Encoded formats (FLAC via the
reference encoder, MP3 via LAME, Ogg Vorbis via ffmpeg, AAC/ALAC/CAF via
afconvert) shell out to tools when they are present; a missing tool skips
its seeds with a printed notice and never fails the run. Every malformed
seed derives from a valid base built in the same run, so the pre-corruption
bytes are always known.

The run ends with a manifest (name, bytes, sha256) on stdout and a list of
the encoders it used or skipped. The corpus README documents each seed.

Each format's builders live in audio_decoder_seeds/ (one module per format,
plus the shared signal and Corpus in common.py); this script only runs them
in a fixed order and writes the result.
"""

from __future__ import annotations

import hashlib
import os
import sys
import tempfile

from audio_decoder_seeds.aiff import build_aiff_seeds
from audio_decoder_seeds.common import Corpus
from audio_decoder_seeds.flac import build_flac_seeds
from audio_decoder_seeds.mp3 import build_mp3_seeds
from audio_decoder_seeds.mp4 import build_coreaudio_seeds
from audio_decoder_seeds.ogg import build_ogg_seeds
from audio_decoder_seeds.wav import build_wav_seeds


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out_dir = argv[1]
    os.makedirs(out_dir, exist_ok=True)
    corpus = Corpus()
    with tempfile.TemporaryDirectory(prefix="pulp-seed-corpus-") as tmp:
        build_wav_seeds(corpus)
        build_aiff_seeds(corpus)
        build_flac_seeds(corpus, tmp)
        build_ogg_seeds(corpus, tmp)
        build_mp3_seeds(corpus, tmp)
        build_coreaudio_seeds(corpus, tmp)

    total = 0
    print(f"{'name':44} {'bytes':>7}  sha256")
    for name in sorted(corpus.files):
        data = corpus.files[name]
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(data)
        total += len(data)
        print(f"{name:44} {len(data):7d}  {hashlib.sha256(data).hexdigest()}")
    print(f"{len(corpus.files)} seeds, {total} bytes total")
    for tool, path in sorted(corpus.tools_used.items()):
        print(f"encoder used: {tool} -> {path}")
    for line in corpus.skipped:
        print(f"skipped: {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
