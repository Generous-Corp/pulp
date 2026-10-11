#!/usr/bin/env python3
"""pulp-ui-frames-observe: fixed-clock frame records for DesignIR and scripted UIs.

Runs the observer against the checked-in fixtures and asserts the properties a
parity harness relies on: the dispatch decision follows the host frame
contract, a hover animation integrates the scheduled dt frame by frame, a
parameter written by the host or by one editor reaches every attached editor
on the same frame, a combo box opens and selects from the keyboard, resize and
detach are observed, a deferred document mounts on a later frame than an
immediate one, two runs are byte-identical, and malformed schedules are refused.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "ui-frames"
SKIP_EXIT = 77


def run(observer: Path, out: Path, *, source: tuple[str, Path], schedule: Path,
        width: int, height: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(observer), source[0], str(source[1]), "--schedule", str(schedule),
         "--width", str(width), "--height", str(height), "--out-dir", str(out)],
        capture_output=True, text=True, encoding="utf-8", check=False)


def observe(observer: Path, out: Path, **kwargs) -> dict:
    result = run(observer, out, **kwargs)
    if result.returncode == SKIP_EXIT:
        print(f"SKIP: {result.stderr.strip()}")
        sys.exit(SKIP_EXIT)
    assert result.returncode == 0, result.stderr
    return json.loads((out / "frames.json").read_text(encoding="utf-8"))


def control(view_record: dict, ident: str) -> dict:
    matches = [s for s in view_record["state"] if s["id"] == ident]
    assert len(matches) == 1, f"{ident}: {view_record['state']}"
    return matches[0]


def check_design_ir(observer: Path, work: Path) -> None:
    schedule = FIXTURES / "controls-ir.schedule.json"
    source = ("--design-ir", FIXTURES / "controls.ir.json")
    log = observe(observer, work / "ir-a", source=source, schedule=schedule, width=320, height=160)
    frames = log["frames"]
    assert len(frames) == len(json.loads(schedule.read_text(encoding="utf-8"))["timestamps"])
    assert all(b["resolved"] for b in log["bindings"]), log["bindings"]
    assert log["inputs"]["mode"] == "design-ir"

    # Idle frames are not dispatched; an input frame is.
    assert frames[1]["views"][0]["dispatched"] is False
    assert frames[2]["views"][0]["dispatched"] is True

    # Hover glow integrates the scheduled dt and keeps the loop alive until it
    # settles: strictly rising, continuous while rising, then idle.
    glow = [control(f["views"][0], "cutoff-knob")["hover_glow"] for f in frames[1:8]]
    assert glow[0] == 0.0 and glow[-1] == 1.0, glow
    assert all(b > a for a, b in zip(glow[:5], glow[1:6])), glow
    assert frames[2]["views"][0]["continuous_after"] is True
    assert frames[7]["views"][0]["dispatched"] is False

    # Host automation reaches the bound control on the frame it is written.
    assert abs(control(frames[14]["views"][0], "cutoff-knob")["value"] - 0.8) < 1e-6

    # A second editor attached later starts from the store, and a drag in it
    # moves the parameter and the first editor on the same frame.
    assert [v["view"] for v in frames[16]["views"]] == [0, 1]
    assert abs(control(frames[16]["views"][1], "cutoff-knob")["value"] - 0.8) < 1e-6
    level_before = frames[17]["params"]["level"]
    assert frames[18]["events"][0]["target"] == "level-fader"
    level_after = frames[18]["params"]["level"]
    assert level_after > level_before
    assert control(frames[18]["views"][0], "level-fader")["value"] == level_after

    # Menu: click opens the combo, Down moves the highlight, Enter selects and
    # writes the parameter, which the other editor shows on the same frame.
    assert control(frames[20]["views"][0], "mode-select")["open"] is True
    assert control(frames[21]["views"][0], "mode-select")["hovered"] == 1
    selected = control(frames[22]["views"][0], "mode-select")
    assert selected["open"] is False and selected["selected"] == 1
    assert frames[22]["params"]["mode"] == 0.5
    assert control(frames[22]["views"][1], "mode-select")["selected"] == 1

    # Resize changes the observed surface; detach removes the second editor.
    assert frames[23]["views"][0]["pixel_size"] == "640x320"
    assert frames[24]["views"][0]["pixel_size"] == "720x360"
    assert [v["view"] for v in frames[26]["views"]] == [0]
    assert log["views"][1]["detached_frame"] == 26
    assert all(v["pixels_sha256"] for f in frames for v in f["views"])
    assert sorted(p.name for p in (work / "ir-a").glob("frame-*.png")) == [
        "frame-v0-f0000.png", "frame-v0-f0004.png", "frame-v0-f0029.png"]

    # Two processes, one schedule: byte-identical records and captures.
    again = run(observer, work / "ir-b", source=source, schedule=schedule, width=320, height=160)
    assert again.returncode == 0, again.stderr
    for name in ["frames.json", *(p.name for p in (work / "ir-a").glob("frame-*.png"))]:
        assert (work / "ir-a" / name).read_bytes() == (work / "ir-b" / name).read_bytes(), name

    # The same events on a 120 Hz clock: a different glow trajectory, the
    # same settled value. A final-frame comparison cannot tell these apart.
    fast = json.loads(schedule.read_text(encoding="utf-8"))
    fast["timestamps"] = [round(10.0 + i / 120, 9) for i in range(len(fast["timestamps"]))]
    fast["nominal_dt"] = round(1 / 120, 9)
    fast_path = work / "fast.schedule.json"
    fast_path.write_text(json.dumps(fast), encoding="utf-8")
    quick = observe(observer, work / "ir-fast", source=source, schedule=fast_path, width=320, height=160)
    fast_glow = [control(f["views"][0], "cutoff-knob")["hover_glow"] for f in quick["frames"][1:8]]
    assert fast_glow[1] < glow[1], (fast_glow, glow)
    assert quick["frames"][-1]["views"][0]["state"] == frames[-1]["views"][0]["state"]


def check_script(observer: Path, work: Path) -> None:
    schedule = FIXTURES / "controls-script.schedule.json"
    source = ("--script", FIXTURES / "controls.ui.js")
    log = observe(observer, work / "script-deferred", source=source, schedule=schedule,
                  width=240, height=160)
    frames = log["frames"]
    first = frames[0]["views"][0]
    # View-first: the first frame precedes the document.
    assert first["readiness"]["document_load_pending"] is True and first["state"] == []
    mounted = frames[1]["views"][0]
    assert mounted["readiness"]["loaded_frame"] == 1 and mounted["readiness"]["loaded_ok"] is True
    status = [control(f["views"][0], "status")["text"] for f in frames[1:8]]
    assert status[0] == "loading" and status[-1] == "ready", status
    # A second editor attached at frame 10 mounts on its own later frame.
    assert frames[11]["views"][1]["readiness"]["loaded_frame"] == 11
    # A drag in editor 0 reaches editor 1 through the bound parameter.
    assert frames[12]["params"]["cutoff"] > 0.5
    assert control(frames[12]["views"][1], "cutoff")["value"] == frames[12]["params"]["cutoff"]
    assert frames[19]["params"]["mode"] == 0.5
    assert control(frames[19]["views"][1], "mode")["selected"] == 1

    immediate = json.loads(schedule.read_text(encoding="utf-8"))
    immediate["readiness"] = "immediate"
    path = work / "immediate.schedule.json"
    path.write_text(json.dumps(immediate), encoding="utf-8")
    now = observe(observer, work / "script-immediate", source=source, schedule=path,
                  width=240, height=160)
    assert now["frames"][0]["views"][0]["readiness"]["loaded_frame"] == 0
    assert now["frames"][0]["views"][0]["state"] != []


def check_refusals(observer: Path, work: Path) -> None:
    source = ("--design-ir", FIXTURES / "controls.ir.json")
    base = json.loads((FIXTURES / "controls-ir.schedule.json").read_text(encoding="utf-8"))

    def refused(mutate, expected: int) -> None:
        doc = json.loads(json.dumps(base))
        mutate(doc)
        path = work / "bad.schedule.json"
        path.write_text(json.dumps(doc), encoding="utf-8")
        result = run(observer, work / "bad", source=source, schedule=path, width=320, height=160)
        assert result.returncode == expected, (result.returncode, result.stderr)

    refused(lambda d: d.update(timestamps=[]), 2)
    refused(lambda d: d.update(schema="other"), 2)
    refused(lambda d: d["events"].append({"frame": 999, "kind": "click"}), 2)
    refused(lambda d: d["events"].append({"frame": 3, "kind": "teleport"}), 1)
    refused(lambda d: d["events"].append({"frame": 3, "view": 4, "kind": "click"}), 1)
    usage = subprocess.run([str(observer), "--design-ir", str(FIXTURES / "controls.ir.json")],
                           capture_output=True, text=True, encoding="utf-8", check=False)
    assert usage.returncode == 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--observer", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pulp-ui-frames-") as tmp:
        work = Path(tmp)
        check_design_ir(args.observer, work)
        check_script(args.observer, work)
        check_refusals(args.observer, work)
    print("ui-frames-observe: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
