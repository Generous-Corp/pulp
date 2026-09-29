#!/usr/bin/env python3
"""Tests for the realtime-performance contract.

Every positive case is a shape measured to stall a live editor: a React commit
per pointer sample, per animation frame, per timer tick, per effect run. Every
negative case is the checklist's remedy for the same code -- refs plus direct
writes, a guarded effect setter, a one-shot timer -- so the gate cannot pass by
flagging everything. The last block is the negative control: the whole suite is
re-run against a gate whose hot-path recognizer is disabled, and it must fail.
"""
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))
import realtime_contract  # noqa: E402
from realtime_contract import check_realtime  # noqa: E402

if "--control" in sys.argv:
    # Blind the hot-path recognizer; the suite below must then fail.
    realtime_contract._hot_regions = lambda scan: iter(())

failures = []


def expect(name, condition, detail=""):
    print(f"  {'ok  ' if condition else 'FAIL'} {name}" +
          ("" if condition else f" {detail}"))
    if not condition:
        failures.append(name)


def rules(src):
    return [(f.rule, f.line) for f in check_realtime(src)]


# ── hot-path setters: flagged ──────────────────────────────────────────────

INLINE_MOVE = """function Panel() {
  const [hover, setHover] = useState(null);
  return <div onPointerMove={(e) => {
    setHover(e.target.id);
  }} />;
}
"""
expect("inline onPointerMove setter is flagged",
       rules(INLINE_MOVE) == [("hot-setter", 4)], rules(INLINE_MOVE))

NAMED_CAPTURE = """const Editor = () => {
  const [cursor, setCursor] = React.useState({ x: 0, y: 0 });
  const handleMove = useCallback((e: PointerEvent): void => {
    setCursor({ x: e.clientX, y: e.clientY });
  }, []);
  return <canvas onPointerMoveCapture={handleMove} />;
};
"""
expect("named useCallback capture handler is resolved",
       rules(NAMED_CAPTURE) == [("hot-setter", 4)], rules(NAMED_CAPTURE))

GUARDED_MOVE = """function Bands() {
  const [hot, setHot] = useState(-1);
  function onMove(e) {
    const next = hitTest(e);
    if (next !== hot) setHot(next);
  }
  return <svg onMouseMove={onMove} />;
}
"""
expect("a 'changed only' guard does not excuse a hot setter",
       rules(GUARDED_MOVE) == [("hot-setter", 5)], rules(GUARDED_MOVE))

INDIRECT = """function Knob() {
  const [value, setValue] = useState(0);
  function apply(v) { setValue(v); }
  function drag(e) { apply(e.movementY); }
  return <div onWheel={drag} />;
}
"""
found = check_realtime(INDIRECT)
expect("a setter reached through local calls is flagged",
       [(f.rule, f.line) for f in found] == [("hot-setter", 3)], found)
expect("the call chain is named",
       found and "via drag -> apply" in found[0].message, found)

RAF = """function Meter() {
  const [level, setLevel] = useState(0);
  useEffect(() => {
    let id = 0;
    function draw() {
      setLevel(readLevel());
      id = requestAnimationFrame(draw);
    }
    id = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(id);
  }, []);
}
"""
expect("a requestAnimationFrame loop setter is flagged",
       rules(RAF) == [("hot-setter", 6)], rules(RAF))

INTERVAL = """function Clock() {
  const [t, setT] = useState(0);
  useEffect(() => {
    const id = setInterval(() => setT(Date.now()), 16);
    return () => clearInterval(id);
  }, []);
}
"""
expect("a setInterval callback setter is flagged",
       rules(INTERVAL) == [("hot-setter", 4)], rules(INTERVAL))

REARM = """function Poll() {
  const [frame, setFrame] = useState(null);
  function tick() {
    setFrame(poll());
    setTimeout(tick, 33);
  }
  tick();
}
"""
expect("a self-re-arming setTimeout setter is flagged",
       rules(REARM) == [("hot-setter", 4)], rules(REARM))

LISTENER = """function Surface({ el }) {
  const [pos, setPos] = useState(0);
  el.addEventListener('pointermove', (e) => { setPos(e.x); });
}
"""
expect("a DOM pointermove listener setter is flagged",
       rules(LISTENER) == [("hot-setter", 3)], rules(LISTENER))

CLASS = """class Old extends React.Component {
  onMove = (e) => { this.setState({ x: e.clientX }); };
  render() { return <div onPointerMove={this.onMove} />; }
}
"""
expect("class setState in a hot handler is flagged",
       rules(CLASS) == [("hot-setter", 2)], rules(CLASS))

# ── fresh-object sync effects: flagged ─────────────────────────────────────

SYNC = """function Macros({ host }) {
  const [macroState, setMacroState] = useState([]);
  const [value, setValue] = useState({});
  useEffect(() => {
    setMacroState(new Array(host.count).fill(0));
    setValue({ ...host.values });
  }, [host.revision]);
}
"""
expect("unguarded fresh-object effect setters are flagged",
       rules(SYNC) == [("fresh-sync", 5), ("fresh-sync", 6)], rules(SYNC))

NO_DEPS = """function P({ bands }) {
  const [rows, setRows] = useState([]);
  useLayoutEffect(() => setRows(bands.map((b) => b.gain)));
}
"""
expect("an every-render effect building a fresh array is flagged",
       rules(NO_DEPS) == [("fresh-sync", 3)], rules(NO_DEPS))

# ── double phase: flagged ──────────────────────────────────────────────────

DOUBLE = """function D() {
  const onMove = (e) => draw(e);
  return <div className="x" onPointerMoveCapture={onMove}
              onPointerMove={onMove} />;
}
"""
expect("one handler on both phases is flagged",
       rules(DOUBLE) == [("double-phase", 3)], rules(DOUBLE))

DOUBLE_DOM = """el.addEventListener('pointermove', onMove, true);
el.addEventListener('pointermove', onMove);
"""
expect("one listener on both phases is flagged",
       rules(DOUBLE_DOM) == [("double-phase", 2)], rules(DOUBLE_DOM))

# ── the checklist's remedies: clean ────────────────────────────────────────

REFS = """function Panel() {
  const hover = useRef(null);
  const readout = useRef(null);
  const [open, setOpen] = useState(false);
  const onMove = (e) => {
    hover.current = e.target.id;
    readout.current.textContent = e.target.id;
    requestRepaint();
  };
  return <div onPointerMove={onMove} onClick={() => setOpen(!open)} />;
}
"""
expect("refs + direct writes are clean", rules(REFS) == [], rules(REFS))

GUARDED_SYNC = """function Macros({ host }) {
  const [macroState, setMacroState] = useState([]);
  useEffect(() => {
    const next = host.values;
    if (!sameValues(next, macroState)) {
      setMacroState([...next]);
    }
    host.ready && setMacroState({ ...host.values });
  }, [host.revision]);
  useEffect(() => { setMacroState([]); }, []);
}
"""
expect("guarded and mount-only effect setters are clean",
       rules(GUARDED_SYNC) == [], rules(GUARDED_SYNC))

ONE_SHOT = """function Toast() {
  const [visible, setVisible] = useState(true);
  useEffect(() => {
    const id = setTimeout(() => setVisible(false), 2000);
    return () => clearTimeout(id);
  }, []);
}
"""
expect("a one-shot setTimeout is clean", rules(ONE_SHOT) == [], rules(ONE_SHOT))

IMPERATIVE = """on('gain', 'change', (v) => { setText('readout', v.toFixed(1)); });
function draw() { setValue('meter', level()); requestAnimationFrame(draw); }
requestAnimationFrame(draw);
"""
expect("imperative bridge calls are not React setters",
       rules(IMPERATIVE) == [], rules(IMPERATIVE))

COMMENTED = """function P() {
  const [x, setX] = useState(0);
  // <div onPointerMove={(e) => setX(e.x)} />
  /* requestAnimationFrame(() => setX(1)); */
  return null;
}
"""
expect("commented-out code is not scanned", rules(COMMENTED) == [], rules(COMMENTED))

HTML = """<!doctype html><html><body><div id="k"></div>
<script>
  const k = document.getElementById('k');
  k.addEventListener('pointermove', (e) => { k.style.left = e.x + 'px'; });
</script></body></html>
"""
expect("a clean HTML panel is clean", rules(HTML) == [], rules(HTML))

DISTINCT_PHASES = """<div onPointerMoveCapture={track} onPointerMove={paint} />"""
expect("different handlers per phase are clean",
       rules(DISTINCT_PHASES) == [], rules(DISTINCT_PHASES))

# ── the single entry point agents run: check_contracts.py ─────────────────
import tempfile  # noqa: E402

with tempfile.TemporaryDirectory() as tmp:
    bad = pathlib.Path(tmp) / "panel.jsx"
    bad.write_text(INLINE_MOVE)
    good = pathlib.Path(tmp) / "clean.jsx"
    good.write_text(REFS)
    entry = HERE / "check_contracts.py"
    run = subprocess.run([sys.executable, str(entry), str(good), str(bad)],
                         capture_output=True, text=True)
    expect("check_contracts exits 1 on a realtime finding", run.returncode == 1,
           run.returncode)
    expect("check_contracts names file:line and the checklist",
           f"realtime   {bad}:4: [hot-setter]" in run.stdout
           and "view-bridge/SKILL.md" in run.stdout, run.stdout)
    expect("check_contracts says which gates it skipped",
           "component  SKIPPED" in run.stdout and "macro      SKIPPED" in run.stdout,
           run.stdout)
    run = subprocess.run([sys.executable, str(entry), str(good)],
                         capture_output=True, text=True)
    expect("check_contracts passes a clean panel", run.returncode == 0
           and "realtime   OK" in run.stdout, run.stdout + run.stderr)

# ── negative control: a gate with its hot-path recognizer disabled ─────────
# The suite above must FAIL when the recognizer cannot see hot regions;
# otherwise the positive expectations prove nothing about the recognizer.
if "--control" not in sys.argv:
    control = subprocess.run(
        [sys.executable, __file__, "--control"], capture_output=True, text=True)
    expect("negative control: suite fails with hot regions disabled",
           control.returncode == 0 and "FAIL inline onPointerMove" in control.stdout
           and "FAIL a requestAnimationFrame loop" in control.stdout,
           control.stdout[-400:])

print(f"\n{'FAILED: ' + ', '.join(failures) if failures else 'all checks passed'}")
if "--control" in sys.argv:
    raise SystemExit(0)  # the parent reads the transcript, not this code
raise SystemExit(1 if failures else 0)
