-- REAPER startup script for the Pulp DAW smoke gpu-fault mode
-- (tools/testing/daw-smoke). It opens the project the harness wrote (render
-- speed "online", i.e. realtime: the only render setting no API call can make),
-- puts the stimulus on a fresh track at 0, inserts the GPU fault-continuity
-- plug-in named by PULP_DAW_SMOKE_FX and floats its editor. Once the harness
-- confirms the fault is armed it renders the track ONLINE: the plug-in runs in
-- realtime with its GPU path live and fires the fault when the playhead crosses
-- the requested sample, so this render is the host's own realtime evidence.
-- It then saves the project with the render speed set to full-speed offline,
-- reopens it (a fresh plug-in instance, which does not fault) and bounces
-- again, as the offline-path/PDC cross-check. This script never judges audio.
--
-- Handshake via PULP_DAW_SMOKE_STATUS: "FX_SHOWN idx=N", "RENDERING_REALTIME",
-- "RENDERING_OFFLINE", then "RENDER_DONE file=<path>" or "RENDER_MISSING". The
-- harness writes PULP_GPU_FAULT_GO once the plug-in has logged its armed fault;
-- without it the script stops at "GO_TIMEOUT".

local fx_name = os.getenv("PULP_DAW_SMOKE_FX") or ""
local status_path = os.getenv("PULP_DAW_SMOKE_STATUS") or "/tmp/pulp_daw_smoke_status.txt"
local go_path = os.getenv("PULP_GPU_FAULT_GO") or ""
local project = os.getenv("PULP_GPU_FAULT_PROJECT") or ""
local realtime = os.getenv("PULP_GPU_FAULT_REALTIME") or ""
local bounce = os.getenv("PULP_GPU_FAULT_BOUNCE") or ""
local render_end = tonumber(os.getenv("PULP_GPU_FAULT_RENDER_END") or "0")
local go_timeout = tonumber(os.getenv("PULP_GPU_FAULT_GO_TIMEOUT") or "60")

local function write_status(s)
  local f = io.open(status_path, "w")
  if f then f:write(s); f:close() end
end

local function file_exists(path)
  local f = io.open(path, "r")
  if f then f:close(); return true end
  return false
end

if project ~= "" then
  reaper.Main_openProject("noprompt:" .. project)
end

-- The stimulus goes in through InsertMedia, as a user would drag it in.
reaper.InsertTrackAtIndex(0, true)
local tr = reaper.GetTrack(0, 0)
if tr ~= nil then
  reaper.SetOnlyTrackSelected(tr)
  reaper.SetEditCurPos(0, false, false)
  reaper.InsertMedia(os.getenv("PULP_GPU_FAULT_STIMULUS") or "", 0)
end
if tr == nil or reaper.CountTrackMediaItems(tr) < 1 then
  write_status("FX_NOT_FOUND stimulus_not_inserted")
  return
end
-- REAPER's anticipative FX processing renders a track ahead of playback in
-- bursts, so the plug-in's callbacks arrive back to back and a GPU lead counted
-- in callbacks is never met: every block is CPU fallback and the GPU path is
-- not exercised at all. The harness turns it off for this track unless asked
-- not to (I_PERFFLAGS &2 = no anticipative FX).
if os.getenv("PULP_GPU_FAULT_ANTICIPATIVE") ~= "1" then
  reaper.SetMediaTrackInfo_Value(tr, "I_PERFFLAGS", 2)
end

local function configure_render(path)
  local dir = path:match("^(.+)/[^/]+$") or "."
  local pattern = (path:match("([^/]+)$") or "render.wav"):gsub("%.wav$", "")
  reaper.GetSetProjectInfo(0, "PROJECT_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FILE", dir, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", pattern, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "evaw", true)
  reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0, true)
  reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", render_end, true)
  reaper.GetSetProjectInfo(0, "RENDER_ADDTOPROJ", 0, true)
end
configure_render(realtime)

local t0 = reaper.time_precise()
local wait_for_go, offline_bounce

-- The offline cross-check: save with full-speed offline rendering, reopen, and
-- bounce. Reopening creates a new plug-in instance, which finds the run
-- directory already claimed and runs as a plain convolver: no fault, no capture.
offline_bounce = function()
  write_status("RENDERING_OFFLINE")
  local saved = (bounce:match("^(.+)/[^/]+$") or ".") .. "/offline.rpp"
  reaper.Main_SaveProjectEx(0, saved, 0)
  local f = io.open(saved, "r")
  local text = f and f:read("*a") or ""
  if f then f:close() end
  text = text:gsub("RENDER_1X %d+", "RENDER_1X 0")
  f = io.open(saved, "w")
  if f then f:write(text); f:close() end
  reaper.Main_openProject("noprompt:" .. saved)
  configure_render(bounce)
  reaper.Main_OnCommand(41824, 0)
  if file_exists(bounce) then
    write_status("RENDER_DONE file=" .. bounce)
  else
    write_status("RENDER_MISSING")
  end
end

wait_for_go = function()
  if go_path ~= "" and file_exists(go_path) then
    write_status("RENDERING_REALTIME")
    -- File: Render project, using the most recent render settings. Online, so
    -- it runs in realtime; it blocks until the render finishes.
    reaper.Main_OnCommand(41824, 0)
    if not file_exists(realtime) then
      write_status("RENDER_MISSING")
      return
    end
    reaper.defer(offline_bounce)
    return
  end
  if reaper.time_precise() - t0 > go_timeout then
    write_status("GO_TIMEOUT")
    return
  end
  reaper.defer(wait_for_go)
end

-- A REAPER whose plug-in cache was written by the killed pre-warm launch can
-- still be loading its plug-in list when this script starts, so a missing FX
-- is retried for a bounded time before it counts as not found.
local insert_deadline = reaper.time_precise() + 20
local function insert_fx()
  local fx = reaper.TrackFX_AddByName(tr, fx_name, false, -1)
  if fx >= 0 then
    reaper.TrackFX_Show(tr, fx, 3)  -- 3 = show the floating FX window
    write_status("FX_SHOWN idx=" .. fx)
    t0 = reaper.time_precise()
    reaper.defer(wait_for_go)
    return
  end
  if reaper.time_precise() > insert_deadline then
    write_status("FX_NOT_FOUND")
    return
  end
  reaper.defer(insert_fx)
end

reaper.defer(insert_fx)
