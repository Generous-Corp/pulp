-- Generic host insertion/editor/parameter readback. Product UI modulation,
-- animation, preset behavior and live audio are separate scenario evidence.
local fx_name = assert(os.getenv('PULP_DAW_SMOKE_FX'))
local status_path = assert(os.getenv('PULP_DAW_SMOKE_STATUS'))
local token = assert(os.getenv('PULP_DAW_SMOKE_TOKEN'))
local project_path = assert(os.getenv('PULP_DAW_SMOKE_PROJECT'))
local param_index = tonumber(os.getenv('PULP_DAW_SMOKE_PARAM_INDEX') or '-1')
local duration = tonumber(os.getenv('PULP_DAW_SMOKE_DURATION') or '8')
local fields = {token=token, fx_shown=0, phase='STARTING', observations=0,
                observed_changes=0, max_error=0, param_index=param_index}
local status_sequence = 0
-- Windows REAPER and the PowerShell wrapper can briefly overlap while the
-- wrapper reads status.txt.  Replacing the file with remove+rename is not
-- atomic on Windows when the reader still has the old handle open: os.remove
-- then fails with `Access is denied`, which used to leave the run forever at
-- AUTOMATION_RUNNING.  Keep the temp file and retry the publication for a
-- bounded interval.  Terminal snapshots get a longer interval so a transient
-- reader overlap cannot discard the only completion record.
local function status(required)
  fields.host_time = reaper.time_precise()
  status_sequence = status_sequence + 1
  -- Never reuse a temporary pathname.  A reader, antivirus scanner, or a
  -- previous failed run can retain the old `.tmp` handle on Windows.
  local temporary_path = status_path..'.tmp.'..tostring(status_sequence)..'.'..token
  local f = assert(io.open(temporary_path,'w'))
  -- Write the token first.  A diagnostic reader that happens to observe a
  -- partially written file can reject it as stale rather than accepting a
  -- snapshot belonging to another run.
  f:write('token='..tostring(fields.token)..'\n')
  for k,v in pairs(fields) do
    if k ~= 'token' then f:write(k..'='..tostring(v)..'\n') end
  end
  f:close()
  local deadline = reaper.time_precise() + (required and 5 or 0.75)
  while true do
    -- On POSIX this succeeds immediately.  Windows cannot replace an open
    -- target, so remove and retry after the reader releases its handle.
    if os.rename(temporary_path,status_path) then return true end
    os.remove(status_path)
    if os.rename(temporary_path,status_path) then return true end
    if reaper.time_precise() >= deadline then
      os.remove(temporary_path)
      if required then error('STATUS_PUBLISH_TIMEOUT') end
      return false
    end
  end
end
local function fail(reason) fields.error=reason;fields.phase='FAILED';status() end
if reaper.CountTracks(0) ~= 0 then fail('NONEMPTY_PROJECT');return end
reaper.InsertTrackAtIndex(0,true)
local tr = reaper.GetTrack(0,0)
if not tr then fail('TRACK_NOT_AVAILABLE');return end
local fx = reaper.TrackFX_AddByName(tr,fx_name,false,-1)
if fx < 0 then fail('FX_NOT_FOUND');return end
fields.param_count = reaper.TrackFX_GetNumParams(tr,fx)
if param_index >= fields.param_count then fail('PARAM_INDEX_OUT_OF_RANGE');return end
fields.fx_inserted=1;fields.phase='FX_INSERTED';status()
local probe_gate=os.getenv('PULP_DAW_SMOKE_PROBE_GATE')
local function show_editor()
reaper.TrackFX_Show(tr,fx,3)
local window=reaper.TrackFX_GetFloatingWindow(tr,fx)
-- Lua/REAPER pointer formatting differs by CRT: Windows commonly omits 0x.
-- Accept explicit userdata/HWND hexadecimal forms, never arbitrary text.
local window_text=tostring(window)
local window_hex=window_text:match('^userdata:%s*0[xX](%x+)$')
  or window_text:match('^userdata:%s*(%x+)$')
  or window_text:match('^%(HWND%*%)%s*0[xX](%x+)$')
  or window_text:match('^%(HWND%*%)%s*(%x+)$')
local window_number=type(window)=='number' and window or (window_hex and tonumber(window_hex,16))
if not window_number or window_number<=0 or window_number~=math.floor(window_number)
  or window_number>9007199254740991 then fail('FLOATING_EDITOR_ABSENT');return end
fields.editor_hwnd=string.format('%.0f',window_number)
fields.fx_shown=1;fields.phase='EDITOR_SHOWN';status()
if param_index < 0 then
  reaper.Main_SaveProjectEx(0,project_path,8)
  fields.state_saved=project_path;fields.phase='EDITOR_READY';status(true);return
end
local started=reaper.time_precise()
local previous=nil
local last_sample=-1
local function pump()
  local elapsed=reaper.time_precise()-started
  if elapsed>=duration then
    reaper.Main_SaveProjectEx(0,project_path,8)
    fields.state_saved=project_path;fields.phase='AUTOMATION_DONE';status(true);return
  end
  if elapsed-last_sample>=0.05 then
    last_sample=elapsed
    local requested=(math.sin(elapsed*2)+1)*0.5
    reaper.TrackFX_SetParamNormalized(tr,fx,param_index,requested)
    local observed=reaper.TrackFX_GetParamNormalized(tr,fx,param_index)
    fields.requested=requested;fields.observed=observed
    fields.max_error=math.max(fields.max_error,math.abs(requested-observed))
    fields.observations=fields.observations+1
    if previous and math.abs(observed-previous)>0.000001 then fields.observed_changes=fields.observed_changes+1 end
    previous=observed;fields.phase='AUTOMATION_RUNNING';status()
  end
  reaper.defer(pump)
end
end
if probe_gate then
  local gate_started=reaper.time_precise()
  local function wait_probe()
    local f=io.open(probe_gate,'r')
    if f then f:close();show_editor();return end
    if reaper.time_precise()-gate_started>60 then fail('MODULE_PROBE_GATE_TIMEOUT');return end
    reaper.defer(wait_probe)
  end
  reaper.defer(wait_probe)
else
  show_editor()
end
