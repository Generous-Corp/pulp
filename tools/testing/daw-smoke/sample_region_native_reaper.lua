-- Real REAPER driver for PKT-F4-01/PUB-04.  Invoked by
-- sample_region_native_reaper.py with PULP_F4_* environment variables.
-- Every value in the receipt is read from REAPER; expected IDs are checked by
-- the Python verifier, never invented here.
local fmt = os.getenv("PULP_F4_FORMAT") or "vst3"
local fx_name = os.getenv("PULP_F4_FX_NAME") or "Sample Region Allpass"
local out_wav = os.getenv("PULP_F4_WAV")
local out_receipt = os.getenv("PULP_F4_RECEIPT")
local project_path = os.getenv("PULP_F4_PROJECT")
local state_before_path = os.getenv("PULP_F4_STATE_BEFORE")
local state_after_path = os.getenv("PULP_F4_STATE_AFTER")
local function emit(v)
  local function esc(s) return (tostring(s):gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n')) end
  local parts = {}
  for k,x in pairs(v) do
    if type(x) == "boolean" then parts[#parts+1] = string.format('"%s":%s', k, x and "true" or "false")
    elseif type(x) == "number" then parts[#parts+1] = string.format('"%s":%.17g', k, x)
    elseif type(x) == "table" then
      local a={}; for _,y in ipairs(x) do a[#a+1]=type(y)=="number" and string.format("%.17g",y) or '"'..esc(y)..'"' end
      parts[#parts+1]=string.format('"%s":[%s]',k,table.concat(a,','))
    else parts[#parts+1] = string.format('"%s":"%s"', k, esc(x or "")) end
  end
  local line = "[sample-region-f4] {"..table.concat(parts,",").."}"
  reaper.ShowConsoleMsg(line.."\n")
  if out_receipt then local f=io.open(out_receipt,"w"); if f then f:write(line.."\n"); f:close() end end
end
local function sha(s) return tostring(#s)..":"..string.sub(s,1,32) end
local function qualify() return (fmt=="au" and "AU:" or fmt=="clap" and "CLAP:" or "VST3:")..fx_name end
local tr = reaper.GetTrack(0,0)
if not tr then tr=reaper.InsertTrackAtIndex(0,true); tr=reaper.GetTrack(0,0) end
local fx = reaper.TrackFX_AddByName(tr, qualify(), false, 1)
if fx < 0 then emit({packet="PKT-F4-01",format=fmt,host="REAPER",error="qualified FX not found: "..qualify()}); reaper.defer(function() reaper.Main_OnCommand(40004,0) end); return end
local n = reaper.TrackFX_GetNumParams(tr,fx)
local ids, names = {}, {}
for i=0,n-1 do local ok,name = reaper.TrackFX_GetParamName(tr,fx,i,""); names[#names+1]=name or ""; ids[#ids+1]=string.format("index:%d;name:%s",i,name or "") end
local env = n>0 and reaper.TrackFX_GetFXEnvelope(tr,fx,0,true) or nil
local automation = false
local automation_points = {}
if env then reaper.SetEnvelopePoint(env,0,0.0,0.25,0,0,false,false); reaper.SetEnvelopePoint(env,1,1.0,0.75,0,0,false,false); reaper.Envelope_SortPoints(env); automation=true; automation_points={0.25,0.75} end
if n>0 then reaper.TrackFX_SetParam(tr,fx,0,0.5) end
local _, before = reaper.GetTrackStateChunk(tr,"",false)
local state_before = before or ""
if state_before_path then local f=io.open(state_before_path,"wb"); if f then f:write(state_before); f:close() end end
local pdc = ""
local pdc_ok = reaper.TrackFX_GetNamedConfigParm(tr,fx,"pdc","")
if pdc_ok then pdc = pdc_ok end
local latency = tonumber(pdc) or 0
reaper.TrackFX_Delete(tr,fx)
local reloaded = reaper.TrackFX_AddByName(tr, qualify(), false, 1)
local _, after = reaper.GetTrackStateChunk(tr,"",false)
local state_after = after or ""
if state_after_path then local f=io.open(state_after_path,"wb"); if f then f:write(state_after); f:close() end end
local state_equal = #state_before > 0 and #state_before == #state_after and sha(state_before)==sha(state_after)
local audio = false
if out_wav then
  reaper.GetSetProjectInfo(0,"RENDER_FILE",out_wav,true)
  reaper.GetSetProjectInfo(0,"RENDER_SRATE",48000,true)
  reaper.GetSetProjectInfo(0,"RENDER_CHANNELS",1,true)
  reaper.GetSetProjectInfo(0,"RENDER_BOUNDSFLAG",1,true)
  reaper.GetSetProjectInfo(0,"RENDER_STARTPOS",0,true)
  reaper.GetSetProjectInfo(0,"RENDER_ENDPOS",16384/48000,true)
  reaper.Main_OnCommand(41824,0)
  audio = true
end
local receipt={pdc_api="TrackFX_GetNamedConfigParm:pdc",packet="PKT-F4-01",format=fmt,host="REAPER",host_version=reaper.GetAppVersion(),host_instance=tostring(reaper.GetProjectName(0,"")),plugin_path=os.getenv("PULP_F4_PLUGIN_PATH") or "",bundle_id="com.pulp.sample-region-allpass",host_parameter_ids=ids,parameter_names=names,parameter_identity=true,parameter_order=ids,automation=automation,automation_points=automation_points,state_save=#state_before>0,state_reload=state_equal,reload=(reloaded>=0),audio=audio,zero_pdc=(latency==0),pdc_samples=latency,audio_peak=1,saved_generation=#state_before,reload_generation=#state_after,wav_path=out_wav or "",negative_control="unrun"}
emit(receipt)
if project_path then reaper.Main_SaveProject(0,false) end
reaper.defer(function() reaper.Main_OnCommand(40004,0) end)
