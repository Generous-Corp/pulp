"""Helpers for REAPER-backed desktop video proof recipes."""

from __future__ import annotations

from pathlib import Path
import shlex
import tempfile
import textwrap
import uuid


DEFAULT_REAPER_APP = "/Applications/REAPER.app/Contents/MacOS/REAPER"


def _lua_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def reaper_plugin_candidates(plugin: str, plugin_format: str) -> list[str]:
    fmt = plugin_format.lower()
    prefixes = {
        "clap": ["", "CLAP: ", "CLAPi: "],
        "vst3": ["", "VST3: ", "VST3i: "],
        "auv2": ["", "AU: ", "AUi: "],
        "auv3": ["", "AU: ", "AUi: "],
        "lv2": ["", "LV2: "],
    }.get(fmt, [""])
    candidates: list[str] = []
    for prefix in prefixes:
        candidate = f"{prefix}{plugin}"
        if candidate not in candidates:
            candidates.append(candidate)
    return candidates


def installed_clap_bundle_status(plugin: str, *, home: str | Path | None = None) -> tuple[bool, str]:
    home_path = Path(home).expanduser() if home is not None else Path.home()
    bundle = home_path / "Library" / "Audio" / "Plug-Ins" / "CLAP" / f"{plugin}.clap"
    executable = bundle / "Contents" / "MacOS" / plugin
    if not bundle.exists():
        return False, f"CLAP bundle is not installed at `{bundle}`."
    if not executable.exists():
        return False, f"CLAP bundle exists but missing executable `{executable}`."
    if not executable.is_file():
        return False, f"CLAP executable path is not a file: `{executable}`."
    return True, f"CLAP bundle executable found at `{executable}`."


def reaper_clap_cache_status(plugin: str, *, home: str | Path | None = None) -> tuple[bool, str]:
    home_path = Path(home).expanduser() if home is not None else Path.home()
    cache = home_path / "Library" / "Application Support" / "REAPER" / "reaper-clap-macos-aarch64.ini"
    if not cache.exists():
        return True, f"REAPER CLAP cache is not present at `{cache}`; REAPER should scan installed CLAP bundles on launch."
    text = cache.read_text(errors="replace")
    header = f"[{plugin}.clap]"
    start = text.find(header)
    if start < 0:
        return True, f"REAPER CLAP cache has not indexed `{plugin}.clap` yet; REAPER should scan it on launch."
    next_header = text.find("\n[", start + len(header))
    section = text[start:] if next_header < 0 else text[start:next_header]
    for line in section.splitlines()[1:]:
        stripped = line.strip()
        if not stripped or stripped.startswith("_="):
            continue
        if "|" in stripped:
            return True, f"REAPER CLAP cache contains descriptor `{stripped}` for `{plugin}.clap`."
    return False, (
        f"REAPER CLAP cache at `{cache}` has a `{plugin}.clap` stanza but no plugin descriptor line. "
        "REAPER will not find this plugin until the CLAP cache is refreshed."
    )


def write_reaper_plugin_editor_recipe(
    *,
    plugin: str,
    plugin_format: str,
    reaper_app: str = DEFAULT_REAPER_APP,
    root_dir: str | Path | None = None,
) -> dict[str, str]:
    """Create a temporary REAPER wrapper command and ReaScript for a proof run."""
    if not plugin:
        raise ValueError("plugin is required")
    if not plugin_format:
        raise ValueError("plugin_format is required")

    root = Path(root_dir) if root_dir is not None else Path(tempfile.gettempdir()) / "pulp-video-proof-reaper"
    run_dir = root / uuid.uuid4().hex[:12]
    run_dir.mkdir(parents=True, exist_ok=True)
    status_path = run_dir / "status.txt"
    lua_path = run_dir / "open-plugin.lua"
    wrapper_path = run_dir / "run-reaper-proof.zsh"
    reaper_stdout_path = run_dir / "reaper.stdout.log"
    reaper_stderr_path = run_dir / "reaper.stderr.log"
    script_stdout_path = run_dir / "script.stdout.log"
    script_stderr_path = run_dir / "script.stderr.log"

    candidates = reaper_plugin_candidates(plugin, plugin_format)
    lua_candidates = ", ".join(_lua_string(candidate) for candidate in candidates)
    lua_path.write_text(
        textwrap.dedent(
            f"""\
            local status_path = {_lua_string(str(status_path))}
            local plugin_name = {_lua_string(plugin)}
            local plugin_format = {_lua_string(plugin_format)}
            local candidates = {{{lua_candidates}}}

            local function log(msg)
              local text = "[pulp-video-proof] " .. tostring(msg)
              reaper.ShowConsoleMsg(text .. "\\n")
              local f = io.open(status_path, "a")
              if f then
                f:write(tostring(msg), "\\n")
                f:close()
              end
            end

            reaper.ClearConsole()
            os.remove(status_path)
            log("starting plugin=" .. plugin_name .. " format=" .. plugin_format)
            reaper.InsertTrackAtIndex(0, true)
            local track = reaper.GetTrack(0, 0)
            if not track then
              log("no track")
              return
            end
            reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "Pulp video proof track", true)
            local fx = -1
            local matched = ""
            for _, candidate in ipairs(candidates) do
              fx = reaper.TrackFX_AddByName(track, candidate, false, -1)
              log("TrackFX_AddByName " .. candidate .. " -> " .. tostring(fx))
              if fx >= 0 then
                matched = candidate
                break
              end
            end
            if fx >= 0 then
              local ok, name = reaper.TrackFX_GetFXName(track, fx)
              log("fx name ok=" .. tostring(ok) .. " name=" .. tostring(name) .. " matched=" .. matched)
              reaper.TrackFX_Show(track, fx, 3)
              reaper.TrackFX_Show(track, fx, 1)
            else
              log("plugin not found")
            end
            reaper.UpdateArrange()
            log("done")
            """
        )
    )

    wrapper_path.write_text(
        textwrap.dedent(
            f"""\
            #!/bin/zsh
            set -u

            REAPER_APP={shlex.quote(reaper_app)}
            STATUS={shlex.quote(str(status_path))}
            SCRIPT={shlex.quote(str(lua_path))}
            REAPER_STDOUT={shlex.quote(str(reaper_stdout_path))}
            REAPER_STDERR={shlex.quote(str(reaper_stderr_path))}
            SCRIPT_STDOUT={shlex.quote(str(script_stdout_path))}
            SCRIPT_STDERR={shlex.quote(str(script_stderr_path))}

            quit_reaper_best_effort() {{
              /usr/bin/osascript -e 'tell application "REAPER" to quit' >/dev/null 2>&1 &
              local quit_pid=$!
              sleep 2
              kill "$quit_pid" >/dev/null 2>&1 || true
              wait "$quit_pid" >/dev/null 2>&1 || true
            }}

            rm -f "$STATUS" "$REAPER_STDOUT" "$REAPER_STDERR" "$SCRIPT_STDOUT" "$SCRIPT_STDERR"
            quit_reaper_best_effort

            "$REAPER_APP" -newinst -nosplash -new >"$REAPER_STDOUT" 2>"$REAPER_STDERR" &
            reaper_pid=$!

            sleep 5
            "$REAPER_APP" -nonewinst "$SCRIPT" >"$SCRIPT_STDOUT" 2>"$SCRIPT_STDERR" &

            sleep 5
            cat "$STATUS" 2>/dev/null || true

            sleep 30
            kill "$reaper_pid" >/dev/null 2>&1 || true
            quit_reaper_best_effort
            """
        )
    )
    wrapper_path.chmod(0o700)
    return {
        "command": str(wrapper_path),
        "run_dir": str(run_dir),
        "lua_script": str(lua_path),
        "status": str(status_path),
        "reaper_app": reaper_app,
    }
