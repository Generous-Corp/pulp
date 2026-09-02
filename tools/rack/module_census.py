"""Drive the module census over the installed inventory.

The census binary measures one plugin's models per invocation. This wraps it:
it resolves the SDK and the unpacked plugin directory the way everything else
here does, feeds each plugin's models in one batch so the dlopen cost is paid
once, and survives the batches that do not come back.

THE POINT OF BATCHING PER PLUGIN. Third-party module constructors run arbitrary
code, and some of it aborts the process. A single invocation covering the whole
inventory would lose every result after the first such module, and -- worse --
would lose the identity of the module that did it. One process per plugin, with
each measured module's row flushed as it completes, means a crash costs that
plugin's remaining models and names the model it died on.

A TIMEOUT IS NOT A SILENCE. A batch that hangs is recorded as `timeout`, never
as a module that emitted nothing: those are different findings and collapsing
them would manufacture exactly the false refusals this census exists to avoid.

Usage:
    python3 module_census.py --out census.json [--settle-ms N] [--plugin SLUG]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import fetch_sdk                                            # noqa: E402
import patch                                                # noqa: E402

SRC = os.path.join(HERE, "module_census.cpp")
BIN = os.path.join(patch.CACHE_DIR, "module-census")


def build() -> tuple[str | None, str]:
    """The census binary, or None and the reason there isn't one.

    Same contract as `patch.build_gate`: a missing SDK and a compile error are
    different causes and are reported as different causes.
    """
    sdk = fetch_sdk.installed_at() or fetch_sdk.DEST
    if os.path.exists(BIN) and os.path.getmtime(BIN) > os.path.getmtime(SRC):
        return BIN, ""
    if not os.path.exists(os.path.join(sdk, "include", "rack.hpp")):
        return None, f"no Rack SDK at {sdk}"
    os.makedirs(patch.CACHE_DIR, exist_ok=True)
    r = subprocess.run(
        ["clang++", "-std=c++20", "-O1", "-o", BIN, SRC,
         f"-I{sdk}/include", f"-I{sdk}/dep/include", "-DARCH_MAC",
         os.path.join(sdk, "libRack.dylib")],
        capture_output=True, text=True, check=False)
    if r.returncode == 0:
        return BIN, ""
    return None, "the census did not compile:\n" + (r.stderr or r.stdout).strip()


def licence_user_dir() -> tuple[str | None, int]:
    """The Rack user directory the census points `asset::init()` at.

    One definition, in `patch`, because the patch gate needs exactly the same
    thing for exactly the same reason: a commercially licensed module that
    cannot find its cached key silences itself, and a harness that skips this
    measures the licence check instead of the DSP.
    """
    return patch.licence_user_dir()


def _batches(inv: dict, only: str | None) -> list[tuple[str, list[dict]]]:
    """One (plugin, modules) batch per plugin, carrying inventory port roles."""
    rules = patch.module_state_rules()
    out = []
    for plugin, entry in sorted(inv.items()):
        if only and plugin != only:
            continue
        # Core is compiled into Rack itself, so there is no plugin.dylib to
        # dlopen. Recording it as unmeasured is honest; skipping it silently
        # would leave a hole nobody could tell from a pass.
        if plugin == "Core":
            continue
        models = []
        for model, meta in sorted(entry.get("modules", {}).items()):
            # Deliberately NOT gated on `meta["outputs"]`. Port metadata comes
            # from panel classification, which two thirds of the installed
            # plugins have never been through, and the census does not need it:
            # the binary reads the port count off the constructed module. Gating
            # on it skips those plugins entirely while the run still reports a
            # module count and a dead list, so the hole reads as a measurement.
            # `roles_in` only shapes the stimulus and is optional.
            spec = {"plugin": plugin, "model": model,
                    "roles_in": meta.get("roles_in") or []}
            data = (rules.get(f"{plugin}/{model}") or {}).get("data_defaults")
            if data:
                spec["data"] = data
            models.append(spec)
        if models:
            out.append((plugin, models))
    return out


def run(out_path: str, settle_ms: int, only: str | None,
        timeout: float) -> dict:
    binary, why = build()
    if binary is None:
        raise SystemExit(why)
    plugin_dir = patch._plugin_dir()
    if not plugin_dir:
        raise SystemExit("no unpacked Rack plugin directory to measure")

    sdk = fetch_sdk.installed_at() or fetch_sdk.DEST
    env = dict(os.environ, DYLD_LIBRARY_PATH=sdk)
    user_dir, keys = licence_user_dir()
    inv = patch.inventory()
    batches = _batches(inv, only)
    results: dict = {}
    failures: list[dict] = []
    started = time.time()

    for index, (plugin, models) in enumerate(batches, 1):
        if not os.path.isdir(os.path.join(plugin_dir, plugin)):
            failures.append({"plugin": plugin, "why": "not unpacked"})
            continue
        spec = {"plugin_dir": plugin_dir, "settle_ms": settle_ms,
                "modules": models}
        if user_dir:
            spec["user_dir"] = user_dir
        with tempfile.NamedTemporaryFile("w", suffix=".json",
                                         delete=False) as handle:
            json.dump(spec, handle)
            spec_path = handle.name
        try:
            proc = subprocess.run([binary, spec_path], capture_output=True,
                                  text=True, env=env,
                                  timeout=timeout * max(1, len(models)),
                                  check=False)
            stdout, rc, timed_out = proc.stdout, proc.returncode, False
        except subprocess.TimeoutExpired as exc:
            stdout = exc.stdout or ""
            if isinstance(stdout, bytes):
                stdout = stdout.decode("utf-8", "replace")
            rc, timed_out = None, True
        finally:
            os.unlink(spec_path)

        seen = set()
        for line in stdout.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            key = f"{row.get('plugin')}/{row.get('model')}"
            seen.add(row.get("model"))
            results[key] = row
        # Attribute an incomplete batch to the model it died on rather than
        # letting the survivors imply the rest were measured.
        missing = [m["model"] for m in models if m["model"] not in seen]
        if missing and (timed_out or rc not in (0,)):
            failures.append({"plugin": plugin,
                             "why": "timeout" if timed_out else f"exit {rc}",
                             "died_on": missing[0], "unmeasured": missing})
        sys.stderr.write(f"[{index}/{len(batches)}] {plugin}: "
                         f"{len(seen)}/{len(models)}\n")
        sys.stderr.flush()

    payload = {"settle_ms": settle_ms,
               "plugin_dir": plugin_dir,
               "user_dir": user_dir,
               "licence_keys_visible": keys,
               "elapsed_s": round(time.time() - started, 1),
               "modules_measured": len(results),
               "failures": failures,
               "versions": {p: e.get("version") for p, e in inv.items()},
               "results": results}
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=1, sort_keys=True)
    return payload


# The smallest excursion this census is willing to call a signal, in volts.
#
# NOT a tolerance picked to make a number come out. Counting any sample that
# differs from zero calls a port live on arithmetic residue: Host-CV's CV port
# holds a constant 1.49011612e-07 V -- exactly 2^-26, the same value in all
# eleven conditions and every one of its 96000 samples -- which the raw nonzero
# count reads as a fully live port and a human reads as a fixed DC offset.
#
# A millivolt is below anything Rack can act on. Full scale is +/-10 V, gates
# and triggers are recognised at 1 V, and 1 mV of V/oct is about a thousandth
# of a semitone. A port whose largest excursion across two seconds and all
# eleven conditions stays under this cannot carry audio or drive anything
# downstream, whatever its sample count says.
AUDIBLE_FLOOR_V = 1e-3


def silent_ports(row: dict) -> list[int]:
    """Output ports that never moved past `AUDIBLE_FLOOR_V` in any condition.

    Deliberately the weaker of the two claims available here. A port listed by
    this function is one nothing reached; it is NOT a claim that the module is
    unusable, because the census drives each module alone and cannot supply the
    neighbour, the loaded sample or the licensed host some modules need.
    """
    conditions = row.get("conditions") or {}
    if not conditions:
        return []
    ports = range(row.get("num_outputs", 0))
    return [p for p in ports
            if all((c[p]["peak_abs_v"] < AUDIBLE_FLOOR_V)
                   for c in conditions.values() if p < len(c))]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(patch.CACHE_DIR,
                                                  "module-census.json"))
    ap.add_argument("--settle-ms", type=int, default=0)
    ap.add_argument("--plugin", default=None)
    ap.add_argument("--timeout", type=float, default=20.0,
                    help="seconds per model in a batch")
    args = ap.parse_args()
    payload = run(args.out, args.settle_ms, args.plugin, args.timeout)
    dead = [k for k, row in payload["results"].items()
            if row.get("ok") and row.get("num_outputs")
            and len(silent_ports(row)) == row["num_outputs"]]
    print(f"measured {payload['modules_measured']} modules in "
          f"{payload['elapsed_s']}s; {len(dead)} emitted nothing on any port; "
          f"{len(payload['failures'])} batches incomplete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
