-- REAPER startup script for the Pulp keyboard-routing smoke.
--
-- Inserts the plugin named by PULP_DAW_SMOKE_FX on a fresh track, floats its
-- editor, and then journals the HOST's own state to PULP_DAW_SMOKE_JOURNAL
-- every defer tick. The Python driver presses keys at the floating editor and
-- reads the journal to decide what the host actually received.
--
-- The journal is the point: whether the editor consumed a key is invisible
-- from inside the editor, and the only unambiguous evidence that Space still
-- reaches the DAW is the DAW's transport moving.
local fx_name = os.getenv("PULP_DAW_SMOKE_FX") or "Pulp Hot-Reload Morph"
local status_path = os.getenv("PULP_DAW_SMOKE_STATUS") or "/tmp/pulp_daw_smoke_status.txt"
local journal_path = os.getenv("PULP_DAW_SMOKE_JOURNAL") or "/tmp/pulp_daw_smoke_journal.txt"

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

-- Rewrite (never append) a single-line snapshot so the driver always reads a
-- complete record: an appending journal can be read mid-write.
local function journal()
  local play = reaper.GetPlayState()          -- bit 0 = playing, bit 1 = paused
  local undo = reaper.Undo_CanUndo2(0) or ""
  local out = io.open(journal_path, "w")
  if out then
    out:write(string.format("play=%d undo=%s", play, undo))
    out:close()
  end
  reaper.defer(journal)
end

reaper.defer(journal)
