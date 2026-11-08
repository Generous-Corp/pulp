#!/usr/bin/env python3
"""Developer-lane cross-check of `pulp audio inspect --json` against ffprobe and soundfile.

Walks a directory of audio files, runs the Pulp inspector on each, and compares
the stream facts it states with what `ffprobe -print_format json` and Python's
`soundfile.info` say about the same file. Both references are optional: a
missing one prints SKIPPED with the reason and the comparison continues with
whatever is present. Disagreements are printed, never resolved: the inspector
reports what the bytes declare, and a reference decoder may legitimately say
something else (a lossy stream's priming, a container that states no length).

Usage:
    audio_inspect_crosscheck.py <dir-or-file> [--pulp <pulp-cpp>] [--ffprobe <path>]
Exit code is 0 when every file agreed or was skipped, 1 when any fact disagreed,
2 when the Pulp CLI could not be found.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

AUDIO_SUFFIXES = {".wav", ".wave", ".rf64", ".w64", ".bwf", ".aif", ".aiff", ".aifc",
                  ".flac", ".ogg", ".oga", ".opus", ".mp3", ".mp4", ".m4a"}


def find_pulp(explicit):
    if explicit:
        return explicit
    here = Path(__file__).resolve()
    for candidate in (here.parents[2] / "build" / "tools" / "cli" / "pulp-cpp",
                      Path.cwd() / "build" / "tools" / "cli" / "pulp-cpp"):
        if candidate.exists():
            return str(candidate)
    return shutil.which("pulp-cpp") or shutil.which("pulp")


def pulp_facts(pulp, path):
    run = subprocess.run([pulp, "audio", "inspect", str(path), "--json"], capture_output=True, text=True, encoding="utf-8")
    if run.returncode not in (0, 2) or not run.stdout.strip():
        return None, f"pulp exit {run.returncode}: {run.stderr.strip()}"
    doc = json.loads(run.stdout)
    stream = doc.get("stream") or {}
    return {
        "container": doc.get("container"),
        "confidence": doc.get("facts_confidence"),
        "sample_rate": stream.get("sample_rate"),
        "channels": stream.get("channels"),
        "bits": stream.get("bits_per_sample"),
        "frames": stream.get("frames"),
        "encoding": stream.get("encoding"),
    }, None


def ffprobe_facts(ffprobe, path):
    if not ffprobe:
        return None, "ffprobe not found"
    run = subprocess.run([ffprobe, "-v", "error", "-print_format", "json", "-show_streams", "-show_format", str(path)],
                         capture_output=True, text=True, encoding="utf-8")
    if run.returncode != 0:
        return None, f"ffprobe refused: {run.stderr.strip()}"
    doc = json.loads(run.stdout or "{}")
    audio = next((s for s in doc.get("streams", []) if s.get("codec_type") == "audio"), None)
    if audio is None:
        return None, "ffprobe found no audio stream"
    frames = audio.get("duration_ts") if audio.get("time_base", "").endswith("/" + str(audio.get("sample_rate"))) else None
    return {
        "sample_rate": int(audio["sample_rate"]) if audio.get("sample_rate") else None,
        "channels": audio.get("channels"),
        "bits": audio.get("bits_per_raw_sample") or audio.get("bits_per_sample") or None,
        "frames": frames,
        "codec": audio.get("codec_name"),
        "format": doc.get("format", {}).get("format_name"),
    }, None


def soundfile_facts(path):
    try:
        import soundfile  # noqa: F401
    except ImportError:
        return None, "python soundfile module not installed"
    import soundfile as sf
    try:
        info = sf.info(str(path))
    except Exception as error:  # soundfile raises RuntimeError on unsupported input
        return None, f"soundfile refused: {error}"
    subtype_bits = {"PCM_S8": 8, "PCM_U8": 8, "PCM_16": 16, "PCM_24": 24, "PCM_32": 32, "FLOAT": 32, "DOUBLE": 64}
    return {
        "sample_rate": info.samplerate,
        "channels": info.channels,
        "bits": subtype_bits.get(info.subtype),
        "frames": info.frames,
        "format": info.format,
    }, None


def compare(name, ours, theirs, keys):
    disagreements = []
    for key in keys:
        mine = ours.get(key)
        other = theirs.get(key)
        if other in (None, 0) or mine in (None, 0):
            continue
        if str(mine) != str(other):
            disagreements.append(f"{key}: pulp={mine} {name}={other}")
    return disagreements


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target")
    parser.add_argument("--pulp")
    parser.add_argument("--ffprobe", default=shutil.which("ffprobe") or ("/usr/local/bin/ffprobe" if os.path.exists("/usr/local/bin/ffprobe") else None))
    args = parser.parse_args()
    pulp = find_pulp(args.pulp)
    if not pulp or not Path(pulp).exists():
        print("ERROR: pulp CLI not found; pass --pulp <path-to-pulp-cpp>", file=sys.stderr)
        return 2
    target = Path(args.target)
    files = [target] if target.is_file() else sorted(p for p in target.rglob("*") if p.suffix.lower() in AUDIO_SUFFIXES)
    any_disagreement = False
    for path in files:
        ours, error = pulp_facts(pulp, path)
        if ours is None:
            print(f"{path.name}: SKIPPED ({error})")
            continue
        line = [f"{path.name}: pulp {ours['container']}/{ours['confidence']} rate={ours['sample_rate']} ch={ours['channels']} bits={ours['bits']} frames={ours['frames']}"]
        for name, (facts, why) in (("ffprobe", ffprobe_facts(args.ffprobe, path)), ("soundfile", soundfile_facts(path))):
            if facts is None:
                line.append(f"  {name}: SKIPPED ({why})")
                continue
            keys = ["sample_rate", "channels", "bits", "frames"]
            diffs = compare(name, ours, facts, keys)
            if diffs:
                any_disagreement = True
                line.append(f"  {name}: DISAGREE " + "; ".join(diffs))
            else:
                line.append(f"  {name}: agree")
        print("\n".join(line))
    return 1 if any_disagreement else 0


if __name__ == "__main__":
    sys.exit(main())
