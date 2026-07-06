-- REAPER startup script for the Pulp DAW smoke (tools/testing/daw-smoke).
-- Inserts the plugin named by PULP_DAW_SMOKE_FX on a fresh track and floats its
-- editor, then writes a status line to PULP_DAW_SMOKE_STATUS so the Python driver
-- knows whether the FX loaded. The driver captures REAPER's stdout (the plugin's
-- reload log) to decide PASS/FAIL. This script does not judge the swap itself.
local fx_name = os.getenv("PULP_DAW_SMOKE_FX") or "Pulp Hot-Reload Morph"
local status_path = os.getenv("PULP_DAW_SMOKE_STATUS") or "/tmp/pulp_daw_smoke_status.txt"

local tr = reaper.GetTrack(0, 0)
if tr == nil then
  reaper.InsertTrackAtIndex(0, true)
  tr = reaper.GetTrack(0, 0)
end

local status = "FX_NOT_FOUND"
if tr ~= nil then
  local fx = reaper.TrackFX_AddByName(tr, fx_name, false, -1)
  if fx >= 0 then
    reaper.TrackFX_Show(tr, fx, 3)  -- 3 = show the floating FX window
    status = "FX_SHOWN idx=" .. fx
  end
end

local f = io.open(status_path, "w")
if f then f:write(status); f:close() end
