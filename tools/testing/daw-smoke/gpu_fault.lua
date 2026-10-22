-- REAPER startup script for the Pulp DAW smoke gpu-fault mode
-- (tools/testing/daw-smoke). It puts the stimulus on a fresh track at 0,
-- inserts the GPU fault-continuity plug-in named by PULP_DAW_SMOKE_FX, floats
-- its editor, waits for the harness to confirm the fault is armed, plays the
-- stimulus in realtime (the plug-in fires the fault when the playhead crosses
-- the requested project sample), then bounces the track offline. This script
-- never judges audio.
--
-- Handshake via PULP_DAW_SMOKE_STATUS: "FX_SHOWN idx=N" once the FX floats,
-- then "PLAYING", "RENDERING", and "RENDER_DONE file=<path>" or
-- "RENDER_MISSING". The harness writes PULP_GPU_FAULT_GO once the plug-in has
-- logged its armed fault; without it the script stops at "GO_TIMEOUT".
--
-- No project file is opened: a project passed on the command line sometimes
-- stopped this script from running, and one opened from here sometimes came
-- up behind a modal "Project Load Warning".

local fx_name = os.getenv("PULP_DAW_SMOKE_FX") or ""
local status_path = os.getenv("PULP_DAW_SMOKE_STATUS") or "/tmp/pulp_daw_smoke_status.txt"
local go_path = os.getenv("PULP_GPU_FAULT_GO") or ""
local bounce = os.getenv("PULP_GPU_FAULT_BOUNCE") or ""
local render_end = tonumber(os.getenv("PULP_GPU_FAULT_RENDER_END") or "0")
local play_end = tonumber(os.getenv("PULP_GPU_FAULT_PLAY_END") or "0")
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

local dir = bounce:match("^(.+)/[^/]+$") or "."
local pattern = (bounce:match("([^/]+)$") or "bounce.wav"):gsub("%.wav$", "")
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

local t0 = reaper.time_precise()
local wait_for_go, play_through

local function bounce_track()
  write_status("RENDERING")
  -- File: Render project, using the most recent render settings. Blocks
  -- until the render finishes.
  reaper.Main_OnCommand(41824, 0)
  if file_exists(bounce) then
    write_status("RENDER_DONE file=" .. bounce)
  else
    write_status("RENDER_MISSING")
  end
end

-- Render only once the stop has fully landed. Rendering straight after
-- OnStopButton() let the pending stop reach the render's own transport part
-- way through, and the rest of the bounce came out silent.
local stopped_at = nil
local function wait_for_stop()
  if reaper.GetPlayState() ~= 0 then
    stopped_at = nil
  elseif stopped_at == nil then
    stopped_at = reaper.time_precise()
  elseif reaper.time_precise() - stopped_at > 1.0 then
    bounce_track()
    return
  end
  reaper.defer(wait_for_stop)
end

play_through = function()
  -- Stop once the playhead has passed the end, or if playback stopped by
  -- itself; a wall-clock bound covers a transport that never moves.
  local playing = reaper.GetPlayState() & 1 == 1
  if (not playing) or reaper.GetPlayPosition() >= play_end
      or reaper.time_precise() - t0 > play_end * 3 + 10 then
    reaper.OnStopButton()
    reaper.defer(wait_for_stop)
    return
  end
  reaper.defer(play_through)
end

wait_for_go = function()
  if go_path ~= "" and file_exists(go_path) then
    write_status("PLAYING")
    reaper.SetEditCurPos(0, false, false)
    reaper.OnPlayButton()
    t0 = reaper.time_precise()
    reaper.defer(play_through)
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
