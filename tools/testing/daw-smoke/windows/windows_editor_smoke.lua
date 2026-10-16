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
local function status()
  fields.host_time = reaper.time_precise()
  local f = assert(io.open(status_path..'.tmp','w'))
  for k,v in pairs(fields) do f:write(k..'='..tostring(v)..'\n') end
  f:close(); os.remove(status_path); assert(os.rename(status_path..'.tmp',status_path))
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
reaper.TrackFX_Show(tr,fx,3)
local window=reaper.TrackFX_GetFloatingWindow(tr,fx)
local window_text=tostring(window)
local window_hex=window_text:match('0x(%x+)')
local window_number=type(window)=='number' and window or (window_hex and tonumber(window_hex,16))
if not window_number or window_number==0 then fail('FLOATING_EDITOR_ABSENT');return end
fields.editor_hwnd=string.format('%.0f',window_number)
fields.fx_shown=1;fields.phase='EDITOR_SHOWN';status()
if param_index < 0 then
  reaper.Main_SaveProjectEx(0,project_path,0)
  fields.state_saved=project_path;fields.phase='EDITOR_READY';status();return
end
local started=reaper.time_precise()
local previous=nil
local last_sample=-1
local function pump()
  local elapsed=reaper.time_precise()-started
  if elapsed>=duration then
    reaper.Main_SaveProjectEx(0,project_path,0)
    fields.state_saved=project_path;fields.phase='AUTOMATION_DONE';status();return
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
reaper.defer(pump)
