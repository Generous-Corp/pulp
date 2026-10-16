-- Windows REAPER editor smoke driver.
-- The PowerShell wrapper owns the process, evidence directory, and cleanup.
-- This script only inserts an explicitly format-qualified FX, opens its editor,
-- and (when requested) moves one parameter through the real host API.

local fx_name = os.getenv("PULP_DAW_SMOKE_FX") or "VST3:Pulp Spectr"
local status_path = os.getenv("PULP_DAW_SMOKE_STATUS") or "pulp_windows_smoke_status.txt"
local param_index = tonumber(os.getenv("PULP_DAW_SMOKE_PARAM_INDEX") or "-1")
local duration = tonumber(os.getenv("PULP_DAW_SMOKE_DURATION") or "8")

local function status(s)
  local f = io.open(status_path, "w")
  if f then f:write(s); f:close() end
end

local tr = reaper.GetTrack(0, 0)
if tr == nil then
  reaper.InsertTrackAtIndex(0, true)
  tr = reaper.GetTrack(0, 0)
end
if tr == nil then status("TRACK_NOT_AVAILABLE"); return end

local fx = reaper.TrackFX_AddByName(tr, fx_name, false, -1)
if fx < 0 then status("FX_NOT_FOUND name=" .. fx_name); return end
reaper.TrackFX_Show(tr, fx, 3) -- floating editor
local param_count = reaper.TrackFX_GetNumParams(tr, fx)
status("FX_SHOWN format-qualified=" .. fx_name .. " param_count=" .. param_count)

-- Drive a real parameter through REAPER's adapter. A missing index is explicit
-- and does not masquerade as a modulation pass.
if param_index >= 0 and param_index < reaper.TrackFX_GetNumParams(tr, fx) then
  local phase = 0
  local last_value = -1.0
  local t0 = reaper.time_precise()
  local function pump()
    local elapsed = reaper.time_precise() - t0
    if elapsed >= duration then
      status("MODULATION_DONE param=" .. param_index .. " param_count=" .. param_count .. " value=" .. last_value)
      return
    end
    phase = phase + 1
    last_value = (math.sin(elapsed * 2.0) + 1.0) * 0.5
    reaper.TrackFX_SetParamNormalized(tr, fx, param_index, last_value)
    if phase >= 2 then
      -- Keep this marker observable even if the host closes the editor during
      -- the final callback; the wrapper waits for MODULATION_DONE below.
      status("MODULATION_RUNNING param=" .. param_index .. " param_count=" .. param_count .. " value=" .. last_value)
    end
    reaper.defer(pump)
  end
  reaper.defer(pump)
else
  status("EDITOR_SHOWN_NO_MODULATION param=" .. param_index)
end
