-- Run with Lua 5.3+ from any OS. This executes the real driver against a small
-- controlled host; it never starts a DAW. argv[1]=driver, argv[2]=existing tempdir.
local driver=assert(arg[1]);local root=assert(arg[2]):gsub('\\','/')..'/'
local original_getenv=os.getenv
local function run(name,index,ignore_write,no_editor)
  local env={PULP_DAW_SMOKE_FX='VST3: Fixture',PULP_DAW_SMOKE_STATUS=root..name..'.status',
    PULP_DAW_SMOKE_TOKEN=name,PULP_DAW_SMOKE_PROJECT=root..name..'.rpp',
    PULP_DAW_SMOKE_PARAM_INDEX=tostring(index),PULP_DAW_SMOKE_DURATION='1'}
  os.getenv=function(k) return env[k] or original_getenv(k) end
  local now=0;local pending;local value=0;local shown=false
  reaper={
    CountTracks=function() return 0 end,InsertTrackAtIndex=function() end,
    GetTrack=function() return {} end,TrackFX_AddByName=function() return 0 end,
    TrackFX_GetNumParams=function() return 1 end,
    TrackFX_Show=function() shown=true end,TrackFX_GetFloatingWindow=function() return not no_editor and 'userdata: 0x1234' or nil end,time_precise=function() return now end,
    TrackFX_SetParamNormalized=function(_,_,_,v) if not ignore_write then value=v end end,
    TrackFX_GetParamNormalized=function() return value end,
    Main_SaveProjectEx=function(_,p) local f=assert(io.open(p,'w'));f:write('fixture');f:close() end,
    defer=function(fn) pending=fn end,
  }
  assert(loadfile(driver))()
  for _=1,1000 do
    if not pending then break end
    local fn=pending;pending=nil;now=now+0.05;fn()
  end
  assert(not pending,'driver failed to terminate within bounded callbacks')
  local f=assert(io.open(env.PULP_DAW_SMOKE_STATUS));local text=f:read('*a');f:close()
  local fields={};for k,v in text:gmatch('([^=\n]+)=([^\n]*)') do fields[k]=v end
  return fields,shown
end
local s=run('automation',0,false)
assert(s.phase=='AUTOMATION_DONE' and s.fx_shown=='1','terminal lost editor handshake')
assert(tonumber(s.observed_changes)>=2 and tonumber(s.max_error)==0,'actual normalized readback missing')
assert(s.state_saved,'saved state missing')
s=run('editor',-1,false)
assert(s.phase=='EDITOR_READY' and s.fx_shown=='1' and s.state_saved,'editor-only terminal incomplete')
s=run('ignored_write',0,true)
assert(tonumber(s.observed_changes)==0 and tonumber(s.max_error)>0.1,'ignored host writes escaped readback')
local shown;s,shown=run('bad_index',1,false)
assert(s.phase=='FAILED' and s.error=='PARAM_INDEX_OUT_OF_RANGE' and not shown,'invalid parameter not rejected')
s=run('absent_editor',-1,false,true)
assert(s.phase=='FAILED' and s.error=='FLOATING_EDITOR_ABSENT','missing editor escaped refusal')
os.getenv=original_getenv
print('PASS: driver terminal, no-automation, ignored-write, invalid-index and absent-editor controls; product runtime NOT EVALUATED')
