-- REAPER startup script for the Pulp DAW smoke sequence-loop-seek mode
-- (tools/testing/daw-smoke). Inserts the plugin named by PULP_DAW_SMOKE_FX on a
-- fresh track, floats its editor, sets a loop region on the timeline, starts
-- playback, and performs a scripted series of seeks: into the loop, across the
-- loop wrap, and out of the loop. The plugin under test emits a per-block
-- transport marker ("[seq-loop] blk host_qn=.. seq_qn=.. active=.. jump=..
-- dropout=..") to REAPER's stdout, which the Python driver captures and scrapes
-- to assert the embedded sequence followed the host playhead across every loop
-- wrap and seek with no dropout. This script only DRIVES the transport; it does
-- NOT judge the result.
--
-- Handshake via PULP_DAW_SMOKE_STATUS: "FX_SHOWN idx=N" once the FX is inserted
-- and floating (the driver waits for this), then "SEEKS_DONE" once the scripted
-- drive completes (the driver waits for this before scraping). "FX_NOT_FOUND"
-- if the plugin could not be inserted (a scan issue → INCONCLUSIVE upstream).
--
-- The drive runs through a single deferred pump so REAPER keeps servicing audio
-- between transport actions; phases fire in order as their wall-clock deadlines
-- (seconds after playback start) pass.

local fx_name = os.getenv("PULP_DAW_SMOKE_FX") or "Pulp Sequence"
local status_path = os.getenv("PULP_DAW_SMOKE_STATUS") or "/tmp/pulp_daw_smoke_status.txt"
local loop_start = tonumber(os.getenv("PULP_DAW_SMOKE_LOOP_START") or "1.0")
local loop_end = tonumber(os.getenv("PULP_DAW_SMOKE_LOOP_END") or "3.0")

local function write_status(s)
  local f = io.open(status_path, "w")
  if f then f:write(s); f:close() end
end

-- Insert the FX on a fresh track and float its editor.
local tr = reaper.GetTrack(0, 0)
if tr == nil then
  reaper.InsertTrackAtIndex(0, true)
  tr = reaper.GetTrack(0, 0)
end
if tr == nil then
  write_status("FX_NOT_FOUND")
  return
end

local fx = reaper.TrackFX_AddByName(tr, fx_name, false, -1)
if fx < 0 then
  write_status("FX_NOT_FOUND")
  return
end
reaper.TrackFX_Show(tr, fx, 3)  -- 3 = show the floating FX window
write_status("FX_SHOWN idx=" .. fx)

-- Configure the loop region: set the time selection + loop points and enable repeat.
reaper.GetSet_LoopTimeRange(true, true, loop_start, loop_end, false)
reaper.GetSetRepeat(1)

-- SetEditCurPos with seekplay=true moves the play cursor mid-playback, so the
-- host transport (and thus the plugin's host playhead) jumps at that block.
local function seek(pos)
  reaper.SetEditCurPos(pos, false, true)
end

-- {seconds_after_start, action}. Ordered; each fires once its deadline passes.
local mid = (loop_start + loop_end) * 0.5
local phases = {
  -- Start playback from inside the loop; let it wrap at least twice.
  { 0.0, function() seek(loop_start); reaper.OnPlayButton() end },
  -- Forward seek toward the loop end (a jump across the loop toward the wrap).
  { 4.5, function() seek(mid) end },
  -- Seek OUT of the loop (before it); playback re-enters the loop at loop_start.
  { 6.0, function() seek(math.max(0.0, loop_start - 0.5)) end },
  -- Seek to just before the loop end so it wraps almost immediately.
  { 8.0, function() seek(loop_end - 0.1) end },
  -- Stop and report the scripted drive complete.
  { 9.5, function() reaper.OnStopButton(); write_status("SEEKS_DONE") end },
}

local t0 = nil
local phase = 0

local function pump()
  local now = reaper.time_precise()
  if t0 == nil then t0 = now end
  local elapsed = now - t0
  while phase < #phases and elapsed >= phases[phase + 1][1] do
    phase = phase + 1
    phases[phase][2]()
  end
  if phase < #phases then
    reaper.defer(pump)
  end
end

reaper.defer(pump)
