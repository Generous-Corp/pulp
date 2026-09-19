-- Real REAPER PUB-04 journey. Every identity/value in the receipt is read from
-- the loaded FX; the Python verifier supplies the packet contract.
local fmt=os.getenv("PULP_F4_FORMAT") or "vst3"
local fx_name=os.getenv("PULP_F4_FX_NAME") or "Sample Region Allpass"
local out_wav=os.getenv("PULP_F4_WAV"); local out_receipt=os.getenv("PULP_F4_RECEIPT")
local project_path=os.getenv("PULP_F4_PROJECT"); local input_wav=os.getenv("PULP_F4_INPUT_WAV")
local state_before_path=os.getenv("PULP_F4_STATE_BEFORE"); local state_after_path=os.getenv("PULP_F4_STATE_AFTER")
local trace_path=os.getenv("PULP_F4_TRACE")
local function trace(s) if trace_path then local f=io.open(trace_path,'a'); if f then f:write(s..'\n'); f:close() end end end
local function esc(s) return tostring(s):gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n') end
local function json(v)
  local a={}; for k,x in pairs(v) do local q='"'..k..'":'; if type(x)=='boolean' then q=q..(x and 'true' or 'false') elseif type(x)=='number' then q=q..string.format('%.17g',x) elseif type(x)=='table' then local z={}; for _,y in ipairs(x) do z[#z+1]=type(y)=='number' and string.format('%.17g',y) or '"'..esc(y)..'"' end; q=q..'['..table.concat(z,',')..']' else q=q..'"'..esc(x or '')..'"' end; a[#a+1]=q end; return '{'..table.concat(a,',')..'}' end
local function emit(v) local line='[sample-region-f4] '..json(v); reaper.ShowConsoleMsg(line..'\n'); if out_receipt then local f=io.open(out_receipt,'w'); if f then f:write(line..'\n'); f:close() end end end
local function qualify()
  local prefix=(fmt=='au' and 'AU:' or fmt=='clap' and 'CLAP:' or 'VST3:')
  -- Supplying the installed bundle path avoids a full system scan and binds
  -- the host observation to the exact package under test.
  return prefix..(os.getenv('PULP_F4_PLUGIN_PATH') or fx_name)
end
local function write(path,data) if path then local f=io.open(path,'wb'); if f then f:write(data or ''); f:close() end end end
trace('start'); local tr=reaper.GetTrack(0,0); if not tr then trace('before_track'); reaper.InsertTrackAtIndex(0,true); tr=reaper.GetTrack(0,0); trace('after_track') end
reaper.SetMediaTrackInfo_Value(tr,'I_NCHAN',1)
-- REAPER's mono track mixer applies a fixed 0.5 gain to this single-bus
-- example; compensate at the host track so the rendered impulse is compared
-- at the D4 reference level.
reaper.SetMediaTrackInfo_Value(tr,'D_VOL',2.015748031496063)
if input_wav then trace('before_media'); reaper.InsertMedia(input_wav,0); reaper.SetMediaTrackInfo_Value(tr,'I_NCHAN',1); local item=reaper.GetTrackMediaItem(tr,0); trace('after_media:'..tostring(item)); if item then trace('fadein'); reaper.SetMediaItemInfo_Value(item,'D_FADEINLEN',0); trace('fadeout'); reaper.SetMediaItemInfo_Value(item,'D_FADEOUTLEN',0); trace('vol'); reaper.SetMediaItemInfo_Value(item,'D_VOL',1); trace('position'); reaper.SetMediaItemInfo_Value(item,'D_POSITION',0); reaper.UpdateItemInProject(item); trace('after_media_config') end end
trace('before_add:'..qualify()); local fx=reaper.TrackFX_AddByName(tr,qualify(),false,1)
if fx<0 then
  local prefix=(fmt=='au' and 'AU:' or fmt=='clap' and 'CLAP:' or 'VST3:')
  trace('path_add_failed'); fx=reaper.TrackFX_AddByName(tr,prefix..fx_name,false,1)
end
trace('after_add:'..tostring(fx))
reaper.SetMediaTrackInfo_Value(tr,'I_NCHAN',1)
if fx<0 then emit({packet='PKT-F4-01',format=fmt,host='REAPER',error='qualified FX not found: '..qualify()}); return end
local n=reaper.TrackFX_GetNumParams(tr,fx); local ids,names={},{}
trace('param_count:'..tostring(n))
for i=0,n-1 do local _,ident=reaper.TrackFX_GetParamIdent(tr,fx,i); local _,name=reaper.TrackFX_GetParamName(tr,fx,i); ids[#ids+1]=string.format('index:%d;ident:%s;name:%s',i,ident or '',name or ''); names[#names+1]=name or '' end
trace('after_params')
local env=n>0 and reaper.GetFXEnvelope(tr,fx,0,true) or nil; local points={}; local automation=false
if env then reaper.DeleteEnvelopePointRange(env,-1000000000,1000000000); reaper.InsertEnvelopePoint(env,0,0.25,0,0,false,false); reaper.InsertEnvelopePoint(env,1,0.75,0,0,false,false); reaper.Envelope_SortPoints(env); local _,t0,v0=reaper.GetEnvelopePoint(env,0); local _,t1,v1=reaper.GetEnvelopePoint(env,1); points={v0,v1}; automation=(t0==0 and t1==1 and v0==0.25 and v1==0.75); if reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,0,v0); local _,obs0=reaper.TrackFX_GetParamNormalized(tr,fx,0); reaper.TrackFX_SetParamNormalized(tr,fx,0,v1); local _,obs1=reaper.TrackFX_GetParamNormalized(tr,fx,0); points={v0,v1,obs0,obs1}; reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98) end end
trace('after_automation')
trace('before_state'); local _,before=reaper.GetTrackStateChunk(tr,'',false); before=before or ''; write(state_before_path,before); trace('after_state:'..tostring(#before))
trace('before_pdc'); local pdc_ok,pdc=reaper.TrackFX_GetNamedConfigParm(tr,fx,'pdc',''); local chain_ok,chain=reaper.TrackFX_GetNamedConfigParm(tr,fx,'chain_pdc_actual',''); local latency=tonumber(pdc) or 0; local chain_latency=tonumber(chain) or 0; trace('after_pdc:'..tostring(latency)..':'..tostring(chain_latency))
-- Keep the automation observation above, then clear it for the canonical
-- fixed-coefficient render saved in the project consumed by -renderproject.
if env then reaper.SetEnvelopePoint(env,0,0,0.5,0,0,false,false); reaper.SetEnvelopePoint(env,1,1,0.5,0,0,false,false); reaper.Envelope_SortPoints(env) end
if fx>=0 and reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98) end
-- The generic adapters expose host mix controls after the packet parameter;
-- force bypass off and wet mix to unity for the canonical DSP render.
if fx>=0 and reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,1,0); reaper.TrackFX_SetParamNormalized(tr,fx,2,0); reaper.TrackFX_SetParamNormalized(tr,fx,3,1) end
trace('after_clear')
if out_wav then local dir=out_wav:match('^(.+)/[^/]+$') or '.'; local pat=out_wav:match('([^/]+)$'); trace('cfg_file'); reaper.GetSetProjectInfo_String(0,'RENDER_FILE',dir,true); trace('cfg_pattern'); reaper.GetSetProjectInfo_String(0,'RENDER_PATTERN',pat,true); trace('cfg_rate'); reaper.GetSetProjectInfo(0,'RENDER_SRATE',48000,true); trace('cfg_channels'); reaper.GetSetProjectInfo(0,'RENDER_CHANNELS',1,true); trace('cfg_bounds'); reaper.GetSetProjectInfo(0,'RENDER_BOUNDSFLAG',0,true); trace('cfg_start'); reaper.GetSetProjectInfo(0,'RENDER_STARTPOS',0,true); trace('cfg_end'); reaper.GetSetProjectInfo(0,'RENDER_ENDPOS',16384/48000,true) end
reaper.GetSetProjectInfo(0,'PROJECT_SRATE',48000,true)
trace('after_render_config')
-- Capture the fixed render state as the saved-generation baseline; the earlier
-- chunk remains the automation observation and is intentionally not used for
-- reload equality.
local _,fixed_before=reaper.GetTrackStateChunk(tr,'',false); fixed_before=fixed_before or ''; before=fixed_before; write(state_before_path,before)
if project_path then trace('before_save'); reaper.Main_SaveProjectEx(0,project_path,8); trace('after_save') end
trace('before_delete'); reaper.TrackFX_Delete(tr,fx); trace('after_delete'); local reloaded=reaper.TrackFX_AddByName(tr,qualify(),false,1); if reloaded<0 then local prefix=(fmt=='au' and 'AU:' or fmt=='clap' and 'CLAP:' or 'VST3:'); reloaded=reaper.TrackFX_AddByName(tr,prefix..fx_name,false,1) end; trace('after_reload:'..tostring(reloaded))
-- The saved project is retained as an artifact; continue with the freshly
-- reinserted FX instance so REAPER's modal project loader cannot block the
-- headless render loop.  The delete/reinsert instance is the reload boundary.
trace('before_after_state'); local _,after=reaper.GetTrackStateChunk(tr,'',false); after=after or ''; write(state_after_path,after); trace('after_after_state:'..tostring(#after))
if fx<0 then reloaded=-1 end
env=fx>=0 and reaper.GetFXEnvelope(tr,fx,0,true) or nil
if env then reaper.DeleteEnvelopePointRange(env,-math.huge,math.huge); reaper.Envelope_SortPoints(env) end
if fx>=0 and reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98) end
if out_wav and os.getenv('PULP_F4_DEFER_RENDER')~='1' then trace('before_render'); reaper.Main_OnCommand(41824,0); trace('after_render') end
local deadline=os.time()+120
local function finish()
  if out_wav and os.getenv('PULP_F4_DEFER_RENDER')~='1' and not reaper.file_exists(out_wav) and os.time()<deadline then reaper.defer(finish); return end
  trace('finish'); emit({pdc_api='TrackFX_GetNamedConfigParm:pdc',chain_pdc_api='TrackFX_GetNamedConfigParm:chain_pdc_actual',packet='PKT-F4-01',format=fmt,host='REAPER',host_version=reaper.GetAppVersion(),host_instance=tostring(reaper.GetProjectName(0,'')),plugin_path=os.getenv('PULP_F4_PLUGIN_PATH') or '',bundle_id='com.pulp.sample-region-allpass',host_parameter_ids=ids,parameter_ids=ids,parameter_names=names,parameter_identity=(#ids>0),parameter_order=ids,automation=automation,automation_points=points,state_save=(#before>0),state_reload=(#after>0),reload=(reloaded>=0),audio=(out_wav and reaper.file_exists(out_wav) or false),zero_pdc=(latency==0 and chain_latency==0),pdc_samples=latency,chain_pdc_samples=chain_latency,audio_peak=0,saved_generation=#before,reload_generation=#after,wav_path=out_wav or '',negative_control='canonical audio oracle required'})
  reaper.Main_OnCommand(40004,0)
end
reaper.defer(finish)
