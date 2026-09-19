-- Real REAPER PUB-04 journey. Every identity/value in the receipt is read from
-- the loaded FX; the Python verifier supplies the packet contract.
local fmt=os.getenv("PULP_F4_FORMAT") or "vst3"
local fx_name=os.getenv("PULP_F4_FX_NAME") or "Sample Region Allpass"
local out_wav=os.getenv("PULP_F4_WAV"); local out_receipt=os.getenv("PULP_F4_RECEIPT")
local project_path=os.getenv("PULP_F4_PROJECT"); local input_wav=os.getenv("PULP_F4_INPUT_WAV")
local state_before_path=os.getenv("PULP_F4_STATE_BEFORE"); local state_after_path=os.getenv("PULP_F4_STATE_AFTER")
local function esc(s) return tostring(s):gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n') end
local function json(v)
  local a={}; for k,x in pairs(v) do local q='"'..k..'":'; if type(x)=='boolean' then q=q..(x and 'true' or 'false') elseif type(x)=='number' then q=q..string.format('%.17g',x) elseif type(x)=='table' then local z={}; for _,y in ipairs(x) do z[#z+1]=type(y)=='number' and string.format('%.17g',y) or '"'..esc(y)..'"' end; q=q..'['..table.concat(z,',')..']' else q=q..'"'..esc(x or '')..'"' end; a[#a+1]=q end; return '{'..table.concat(a,',')..'}' end
local function emit(v) local line='[sample-region-f4] '..json(v); reaper.ShowConsoleMsg(line..'\n'); if out_receipt then local f=io.open(out_receipt,'w'); if f then f:write(line..'\n'); f:close() end end end
local function qualify() return (fmt=='au' and 'AU:' or fmt=='clap' and 'CLAP:' or 'VST3:')..fx_name end
local function write(path,data) if path then local f=io.open(path,'wb'); if f then f:write(data or ''); f:close() end end end
local tr=reaper.GetTrack(0,0); if not tr then reaper.InsertTrackAtIndex(0,true); tr=reaper.GetTrack(0,0) end
if input_wav then local item=reaper.InsertMedia(input_wav,0); if item then reaper.SetMediaItemLength(item,16384/48000,true) end end
local fx=reaper.TrackFX_AddByName(tr,qualify(),false,1)
if fx<0 then emit({packet='PKT-F4-01',format=fmt,host='REAPER',error='qualified FX not found: '..qualify()}); return end
local n=reaper.TrackFX_GetNumParams(tr,fx); local ids,names={},{}
for i=0,n-1 do local _,ident=reaper.TrackFX_GetParamIdent(tr,fx,i); local _,name=reaper.TrackFX_GetParamName(tr,fx,i); ids[#ids+1]=string.format('index:%d;ident:%s;name:%s',i,ident or '',name or ''); names[#names+1]=name or '' end
local env=n>0 and reaper.GetFXEnvelope(tr,fx,0,true) or nil; local points={}; local automation=false
if env then reaper.DeleteEnvelopePointRange(env,-math.huge,math.huge); reaper.InsertEnvelopePoint(env,0,0.25,0,0,false,false); reaper.InsertEnvelopePoint(env,1,0.75,0,0,false,false); reaper.Envelope_SortPoints(env); local _,t0,v0=reaper.GetEnvelopePoint(env,0); local _,t1,v1=reaper.GetEnvelopePoint(env,1); points={v0,v1}; automation=(t0==0 and t1==1 and v0==0.25 and v1==0.75); if reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,0,v0); local _,obs0=reaper.TrackFX_GetParamNormalized(tr,fx,0); reaper.TrackFX_SetParamNormalized(tr,fx,0,v1); local _,obs1=reaper.TrackFX_GetParamNormalized(tr,fx,0); points={v0,v1,obs0,obs1}; reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98) end end
local _,before=reaper.GetTrackStateChunk(tr,'',false); before=before or ''; write(state_before_path,before)
local pdc_ok,pdc=reaper.TrackFX_GetNamedConfigParm(tr,fx,'pdc',''); local chain_ok,chain=reaper.TrackFX_GetNamedConfigParm(tr,fx,'chain_pdc_actual',''); local latency=tonumber(pdc) or 0; local chain_latency=tonumber(chain) or 0
if project_path then reaper.Main_SaveProjectEx(0,project_path,8) end
reaper.TrackFX_Delete(tr,fx); local reloaded=reaper.TrackFX_AddByName(tr,qualify(),false,1)
if project_path then reaper.Main_openProject('noprompt:'..project_path); tr=reaper.GetTrack(0,0); fx=reaper.TrackFX_GetByName(tr,qualify(),false) end
local _,after=reaper.GetTrackStateChunk(tr,'',false); after=after or ''; write(state_after_path,after)
if fx<0 then reloaded=-1 end
env=fx>=0 and reaper.GetFXEnvelope(tr,fx,0,true) or nil
if env then reaper.DeleteEnvelopePointRange(env,-math.huge,math.huge); reaper.Envelope_SortPoints(env) end
if fx>=0 and reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98) end
if out_wav then local dir=out_wav:match('^(.+)/[^/]+$') or '.'; local pat=out_wav:match('([^/]+)$'); reaper.GetSetProjectInfo(0,'RENDER_FILE',dir,true); reaper.GetSetProjectInfo(0,'RENDER_PATTERN',pat,true); reaper.GetSetProjectInfo(0,'RENDER_SRATE',48000,true); reaper.GetSetProjectInfo(0,'RENDER_CHANNELS',1,true); reaper.GetSetProjectInfo(0,'RENDER_BOUNDSFLAG',0,true); reaper.GetSetProjectInfo(0,'RENDER_STARTPOS',0,true); reaper.GetSetProjectInfo(0,'RENDER_ENDPOS',16384/48000,true); reaper.Main_OnCommand(41824,0) end
local deadline=os.time()+120
local function finish()
  if out_wav and not reaper.file_exists(out_wav) and os.time()<deadline then reaper.defer(finish); return end
  emit({pdc_api='TrackFX_GetNamedConfigParm:pdc',chain_pdc_api='TrackFX_GetNamedConfigParm:chain_pdc_actual',packet='PKT-F4-01',format=fmt,host='REAPER',host_version=reaper.GetAppVersion(),host_instance=tostring(reaper.GetProjectName(0,'')),plugin_path=os.getenv('PULP_F4_PLUGIN_PATH') or '',bundle_id='com.pulp.sample-region-allpass',host_parameter_ids=ids,parameter_names=names,parameter_identity=(#ids>0),parameter_order=ids,automation=automation,automation_points=points,state_save=(#before>0),state_reload=(#after>0),reload=(reloaded>=0),audio=(out_wav and reaper.file_exists(out_wav) or false),zero_pdc=(latency==0 and chain_latency==0),pdc_samples=latency,chain_pdc_samples=chain_latency,audio_peak=0,saved_generation=#before,reload_generation=#after,wav_path=out_wav or '',negative_control='canonical audio oracle required')
  reaper.Main_OnCommand(40004,0)
end
reaper.defer(finish)
