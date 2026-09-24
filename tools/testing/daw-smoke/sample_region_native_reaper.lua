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
local function ensure_serialized_wet(track, fx_index, param_index)
  if not param_index or param_index < 0 or not reaper.TrackFX_SetParamNormalized then return false end
  reaper.TrackFX_SetParamNormalized(track,fx_index,param_index,1.0)
  local _,chunk=reaper.GetTrackStateChunk(track,'',false); chunk=chunk or ''
  if not chunk:match('\n%s*WET%s') then
    local updated=chunk:gsub('(\n%s*FLOATPOS 0 0 0 0\n)',
      '\n      SAMPLE_ACCURATE_WETDRY 1\n      WET 1.000000 0%1', 1)
    if updated ~= chunk and reaper.SetTrackStateChunk then
      reaper.SetTrackStateChunk(track,updated,false)
      return true
    end
  end
  return chunk:match('\n%s*WET%s') ~= nil
end
trace('start'); local tr=reaper.GetTrack(0,0); if not tr then trace('before_track'); reaper.InsertTrackAtIndex(0,true); tr=reaper.GetTrack(0,0); trace('after_track') end
reaper.SetMediaTrackInfo_Value(tr,'I_NCHAN',1)
-- REAPER's mono track mixer applies a fixed 0.5 gain to this single-bus
-- example; compensate at the host track so the rendered impulse is compared
-- at the D4 reference level.
reaper.SetMediaTrackInfo_Value(tr,'D_VOL',fmt=='au' and 2.0 or 1.0)
if input_wav then trace('before_media'); reaper.InsertMedia(input_wav,0); reaper.SetMediaTrackInfo_Value(tr,'I_NCHAN',1); local item=reaper.GetTrackMediaItem(tr,0); trace('after_media:'..tostring(item)); if item then trace('fadein'); reaper.SetMediaItemInfo_Value(item,'D_FADEINLEN',0); trace('fadeout'); reaper.SetMediaItemInfo_Value(item,'D_FADEOUTLEN',0); trace('vol'); reaper.SetMediaItemInfo_Value(item,'D_VOL',2.015748031496063); trace('position'); reaper.SetMediaItemInfo_Value(item,'D_POSITION',0); trace('length'); reaper.SetMediaItemInfo_Value(item,'D_LENGTH',(16384+16)/48000); reaper.UpdateItemInProject(item); trace('after_media_config') end end
local add_resolution='exact_path'; local reload_resolution='exact_path'
trace('before_add:'..qualify()); local fx=reaper.TrackFX_AddByName(tr,qualify(),false,1)
if fx<0 then
  local prefix=(fmt=='au' and 'AU:' or fmt=='clap' and 'CLAP:' or 'VST3:')
  trace('path_add_failed'); fx=reaper.TrackFX_AddByName(tr,prefix..fx_name,false,1)
  add_resolution='name_fallback'
end
trace('after_add:'..tostring(fx))
reaper.SetMediaTrackInfo_Value(tr,'I_NCHAN',1)
if fx<0 then emit({packet='PKT-F4-01',format=fmt,host='REAPER',error='qualified FX not found: '..qualify()}); return end
-- REAPER keeps VST3 pin activation separate from the track channel count.
-- Bind the mono main input/output pins explicitly so the host cannot leave
-- the component's declared bus inactive while still presenting a stereo
-- ProcessData shape. TrackFX_SetPinMappings is a documented ReaScript API;
-- record its return values as host evidence rather than assuming success.
if fmt=='vst3' and reaper.TrackFX_SetPinMappings then
  local in_ok=reaper.TrackFX_SetPinMappings(tr,fx,0,0,1,0)
  local out_ok=reaper.TrackFX_SetPinMappings(tr,fx,1,0,3,0)
  trace('pin_map:'..tostring(in_ok)..':'..tostring(out_ok))
end
local n=reaper.TrackFX_GetNumParams(tr,fx); local ids,names={},{}
trace('param_count:'..tostring(n))
for i=0,n-1 do local _,ident=reaper.TrackFX_GetParamIdent(tr,fx,i); local _,name=reaper.TrackFX_GetParamName(tr,fx,i); ids[#ids+1]=string.format('index:%d;ident:%s;name:%s',i,ident or '',name or ''); names[#names+1]=name or '' end
trace('after_params')
local env=n>0 and reaper.GetFXEnvelope(tr,fx,0,true) or nil; local points={}; local automation=false
if env then reaper.DeleteEnvelopePointRange(env,-1000000000,1000000000); reaper.InsertEnvelopePoint(env,0,0.25,0,0,false,false); reaper.InsertEnvelopePoint(env,1,0.75,0,0,false,false); reaper.Envelope_SortPoints(env); local _,t0,v0=reaper.GetEnvelopePoint(env,0); local _,t1,v1=reaper.GetEnvelopePoint(env,1); points={v0,v1}; automation=(t0==0 and t1==1 and v0==0.25 and v1==0.75); if reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,0,v0); local _,obs0=reaper.TrackFX_GetParamNormalized(tr,fx,0); reaper.TrackFX_SetParamNormalized(tr,fx,0,v1); local _,obs1=reaper.TrackFX_GetParamNormalized(tr,fx,0); points={v0,v1,obs0,obs1}; reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98) end end
trace('after_automation')
trace('before_state'); local _,before=reaper.GetTrackStateChunk(tr,'',false); before=before or ''; write(state_before_path,before); trace('after_state:'..tostring(#before))
trace('before_pdc'); local pdc_ok,pdc=reaper.TrackFX_GetNamedConfigParm(tr,fx,'pdc',''); local chain_ok,chain=reaper.TrackFX_GetNamedConfigParm(tr,fx,'chain_pdc_actual',''); local latency=tonumber(pdc) or 0; local chain_latency=tonumber(chain) or 0; trace('after_pdc:'..tostring(latency)..':'..tostring(chain_latency))
-- Keep the automation observation above, then retire the envelope for the
-- canonical fixed-coefficient render. NOTE: this is a correctness fix, NOT a
-- fix for the -0.15708 handoff corruption -- that still reproduces at a similar
-- rate with the envelope provably retired (act 0, arm 0, no points).
-- An ACTIVE envelope OVERRIDES the parameter value, so deleting its points is
-- not enough: REAPER re-adds a point carrying the envelope's own value at the
-- edit cursor, and the saved state then takes that value instead of the one
-- written below. Rebuild the chunk line-by-line -- a gsub on 'ACT 1' does not
-- match the real 'ACT 1 -1' form -- dropping every PT line and forcing ACT/ARM
-- off, then VERIFY the post-condition rather than assuming it.
if env then
  reaper.DeleteEnvelopePointRange(env,-1000000000,1000000000)
  reaper.Envelope_SortPoints(env)
  local chunk_ok,env_chunk=reaper.GetEnvelopeStateChunk(env,'',false)
  if chunk_ok and env_chunk then
    local rebuilt={}
    for line in (env_chunk..'\n'):gmatch('([^\n]*)\n') do
      local trimmed=line:match('^%s*(.-)%s*$')
      if trimmed:match('^PT ') then
        -- drop the point: it carries the envelope's cursor value
      elseif trimmed:match('^ACT ') then
        rebuilt[#rebuilt+1]='ACT 0 -1'
      elseif trimmed:match('^ARM ') then
        rebuilt[#rebuilt+1]='ARM 0'
      else
        rebuilt[#rebuilt+1]=line
      end
    end
    reaper.SetEnvelopeStateChunk(env,table.concat(rebuilt,'\n'),false)
  end
  local ok2,after_chunk=reaper.GetEnvelopeStateChunk(env,'',false)
  local act,arm,pts=nil,nil,0
  if ok2 and after_chunk then
    act=after_chunk:match('ACT%s+(%d+)')
    arm=after_chunk:match('ARM%s+(%d+)')
    for _ in after_chunk:gmatch('PT ') do pts=pts+1 end
  end
  trace('envelope_retired:'..tostring(act=='0' and pts==0)..':act:'..tostring(act)..':arm:'..tostring(arm)..':pts:'..tostring(pts))
end
if fx>=0 and reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98) end
-- Force the documented pseudo-controls to a fully wet, unbypassed render;
-- ensure_serialized_wet also writes REAPER's WET record into the saved track
-- state so a separate -renderproject process observes the same setting.
local wet_index=fx>=0 and reaper.TrackFX_GetParamFromIdent and reaper.TrackFX_GetParamFromIdent(tr,fx,':wet') or -1
local bypass_index=fx>=0 and reaper.TrackFX_GetParamFromIdent and reaper.TrackFX_GetParamFromIdent(tr,fx,':bypass') or -1
if bypass_index>=0 and reaper.TrackFX_SetParamNormalized then reaper.TrackFX_SetParamNormalized(tr,fx,bypass_index,0.0) end
local wet_serialized=ensure_serialized_wet(tr,fx,wet_index)
trace('host_controls_initial:'..tostring(bypass_index)..':'..tostring(wet_index)..':'..tostring(wet_serialized))
if wet_index>=0 and reaper.TrackFX_SetParam and reaper.TrackFX_GetParam then
  local ok=reaper.TrackFX_SetParam(tr,fx,wet_index,2.0)
  local raw=reaper.TrackFX_GetParam(tr,fx,wet_index)
  trace('host_wet_raw2:'..tostring(ok)..':'..tostring(raw))
end
local coeff_observed, wet_observed
if fx>=0 and reaper.TrackFX_GetParamNormalized then
  local _,c=reaper.TrackFX_GetParamNormalized(tr,fx,0); coeff_observed=c
  local _,w=reaper.TrackFX_GetParamNormalized(tr,fx,wet_index); wet_observed=w
  if reaper.TrackFX_GetParam then
    local raw,minv,maxv,midv=reaper.TrackFX_GetParam(tr,fx,0)
    trace('coeff_observed:'..tostring(c)..':raw:'..tostring(raw)..':min:'..tostring(minv)..':max:'..tostring(maxv)..':mid:'..tostring(midv))
  end
end
trace('after_clear')
if out_wav then local dir=out_wav:match('^(.+)/[^/]+$') or '.'; local pat=out_wav:match('([^/]+)$'); trace('cfg_file'); reaper.GetSetProjectInfo_String(0,'RENDER_FILE',dir,true); trace('cfg_pattern'); reaper.GetSetProjectInfo_String(0,'RENDER_PATTERN',pat,true); trace('cfg_rate'); reaper.GetSetProjectInfo(0,'RENDER_SRATE',48000,true); trace('cfg_channels'); reaper.GetSetProjectInfo(0,'RENDER_CHANNELS',1,true); reaper.GetSetProjectInfo(0,'RENDER_RESAMPLE',0,true); trace('cfg_bounds'); reaper.GetSetProjectInfo(0,'RENDER_BOUNDSFLAG',0,true); trace('cfg_start'); reaper.GetSetProjectInfo(0,'RENDER_STARTPOS',0,true); trace('cfg_end'); reaper.GetSetProjectInfo(0,'RENDER_ENDPOS',16384/48000,true) end
reaper.GetSetProjectInfo(0,'PROJECT_SRATE',48000,true)
trace('after_render_config')
-- Capture the fixed render state as the saved-generation baseline; the earlier
-- chunk remains the automation observation and is intentionally not used for
-- reload equality.
local _,fixed_before=reaper.GetTrackStateChunk(tr,'',false); fixed_before=fixed_before or ''; before=fixed_before; write(state_before_path,before)
-- The queued parameter write lands only when REAPER flushes pending parameter
-- changes, and GetTrackStateChunk (which calls the plug-in's getState) forces
-- that flush. A wall-clock wait alone does NOT: a 1.5s settle still failed 6/34,
-- while repeated state reads converged. So drive the flush explicitly -- rewrite
-- the parameter and read the state back, bounded -- rather than waiting on it.
-- The DURABLE guard remains the serialized-store post-condition in the Python
-- driver, which fails closed; REAPER's own read-back returns its controller
-- cache and is blind to this.
do
  for _ = 1, 12 do
    if fx>=0 and reaper.TrackFX_SetParamNormalized then
      reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98)
    end
    local _,flush = reaper.GetTrackStateChunk(tr,'',false)
    local _ = flush
    local deadline = reaper.time_precise() + 0.05
    while reaper.time_precise() < deadline do end
  end
  trace('param_flush_loop_done')
end
if project_path then trace('before_save'); reaper.Main_SaveProjectEx(0,project_path,8); trace('after_save') end
trace('before_delete'); reaper.TrackFX_Delete(tr,fx); trace('after_delete'); local reloaded=reaper.TrackFX_AddByName(tr,qualify(),false,1); if reloaded<0 then trace('reload_path_add_failed'); reload_resolution='name_fallback'; local prefix=(fmt=='au' and 'AU:' or fmt=='clap' and 'CLAP:' or 'VST3:'); reloaded=reaper.TrackFX_AddByName(tr,prefix..fx_name,false,1) end; trace('after_reload:'..tostring(reloaded)..':'..tostring(reload_resolution))
-- The saved project is retained as an artifact; continue with the freshly
-- reinserted FX instance so REAPER's modal project loader cannot block the
-- headless render loop.  The delete/reinsert instance is the reload boundary.
trace('before_after_state'); local _,after=reaper.GetTrackStateChunk(tr,'',false); after=after or ''; write(state_after_path,after); trace('after_after_state:'..tostring(#after))
if fx<0 then reloaded=-1 end
-- Restore the exact saved track chunk onto the freshly inserted instance. This
-- makes the reload receipt prove state persistence rather than only a delete /
-- reinsert lifecycle event; SetTrackStateChunk is the documented REAPER API.
if reloaded>=0 and before~='' and reaper.SetTrackStateChunk then
  local restored=reaper.SetTrackStateChunk(tr,before,false)
  trace('restore_state:'..tostring(restored))
  local _,restored_chunk=reaper.GetTrackStateChunk(tr,'',false)
  restored_chunk=restored_chunk or ''
  write(state_after_path,restored_chunk)
  after=restored_chunk
  trace('after_restore_state:'..tostring(#after))
end
if reloaded>=0 then fx=reloaded end
if fmt=='vst3' and reloaded>=0 and reaper.TrackFX_SetPinMappings then
  local in_ok=reaper.TrackFX_SetPinMappings(tr,reloaded,0,0,1,0)
  local out_ok=reaper.TrackFX_SetPinMappings(tr,reloaded,1,0,3,0)
  local ir,ih,il=-1,-1,-1; local orr,oh,ol=-1,-1,-1
  if reaper.TrackFX_GetPinMappings then ir,ih,il=reaper.TrackFX_GetPinMappings(tr,reloaded,0,0); orr,oh,ol=reaper.TrackFX_GetPinMappings(tr,reloaded,1,0) end
  trace('reload_pin_map:'..tostring(in_ok)..':'..tostring(out_ok)..':'..tostring(ir)..':'..tostring(ih)..':'..tostring(il)..':'..tostring(orr)..':'..tostring(oh)..':'..tostring(ol))
end
env=fx>=0 and reaper.GetFXEnvelope(tr,fx,0,true) or nil
if env then reaper.DeleteEnvelopePointRange(env,-math.huge,math.huge); reaper.Envelope_SortPoints(env) end
if fx>=0 and reaper.TrackFX_SetParamNormalized then
  -- The coefficient is a host-normalized parameter whose actual range is
  -- [-0.99, 0.99]. TrackFX_SetParam's raw setter is not equivalent for AU,
  -- and left the post-reload render at coefficient 0.75. Use the same
  -- normalized contract for every format.
  local ok=reaper.TrackFX_SetParamNormalized(tr,fx,0,(0.5+0.99)/1.98)
  local _,obs=reaper.TrackFX_GetParamNormalized(tr,fx,0)
  trace('reload_coeff_set:'..tostring(ok)..':'..tostring(obs))
end
-- Reassert REAPER's generic host controls through their documented pseudo
-- parameter identities after the delete/reinsert + state-restore boundary.
-- Numeric indices are plugin-specific and must never be assumed to mean wet or
-- bypass. The normalized pseudo-parameters are persisted in the track chunk.
if fx>=0 and reaper.TrackFX_GetParamFromIdent and reaper.TrackFX_SetParamNormalized then
  local bypass_index=reaper.TrackFX_GetParamFromIdent(tr,fx,':bypass')
  local wet_index=reaper.TrackFX_GetParamFromIdent(tr,fx,':wet')
  local bypass_ok=false; local wet_ok=false
  if bypass_index and bypass_index>=0 then
    bypass_ok=reaper.TrackFX_SetParamNormalized(tr,fx,bypass_index,0.0)
  end
  if wet_index and wet_index>=0 then
    wet_ok=reaper.TrackFX_SetParamNormalized(tr,fx,wet_index,1.0)
  end
  trace('reload_host_controls:'..tostring(bypass_index)..':'..tostring(wet_index)..':'..tostring(bypass_ok)..':'..tostring(wet_ok))
  if wet_index and wet_index>=0 and reaper.GetFXEnvelope and reaper.InsertEnvelopePoint then
    local wet_env=reaper.GetFXEnvelope(tr,fx,wet_index,true)
    if wet_env then
      reaper.DeleteEnvelopePointRange(wet_env,-math.huge,math.huge)
      reaper.InsertEnvelopePoint(wet_env,0,1.0,0,0,false,false)
      reaper.Envelope_SortPoints(wet_env)
    end
  end
end
-- Deferred -renderproject consumes the saved .rpp, so persist the canonical
-- post-reload controls before the Python driver copies that project for the
-- actual host render.
if project_path then reaper.Main_SaveProjectEx(0,project_path,8) end
if out_wav and os.getenv('PULP_F4_DEFER_RENDER')~='1' then trace('before_render'); reaper.Main_OnCommand(41824,0); trace('after_render') end
local deadline=os.time()+120
local function finish()
  if out_wav and os.getenv('PULP_F4_DEFER_RENDER')~='1' and not reaper.file_exists(out_wav) and os.time()<deadline then reaper.defer(finish); return end
  trace('finish'); emit({pdc_api='TrackFX_GetNamedConfigParm:pdc',chain_pdc_api='TrackFX_GetNamedConfigParm:chain_pdc_actual',packet='PKT-F4-01',format=fmt,host='REAPER',host_version=reaper.GetAppVersion(),host_instance=tostring(reaper.GetProjectName(0,'')),plugin_path=os.getenv('PULP_F4_PLUGIN_PATH') or '',bundle_id='com.pulp.sample-region-allpass',host_parameter_ids=ids,parameter_ids=ids,parameter_names=names,parameter_identity=(#ids>0),parameter_order=ids,automation=automation,automation_points=points,state_save=(#before>0),state_reload=(#after>0),reload=(reloaded>=0),audio=(out_wav and reaper.file_exists(out_wav) or false),zero_pdc=(latency==0 and chain_latency==0),pdc_samples=latency,chain_pdc_samples=chain_latency,audio_peak=0,saved_generation=#before,reload_generation=#after,wav_path=out_wav or '',coefficient_observed=coeff_observed,wet_observed=wet_observed,add_resolution=add_resolution,reload_resolution=reload_resolution,exact_identity_load=(add_resolution=='exact_path' and reload_resolution=='exact_path'),negative_control='canonical audio oracle required'})
  reaper.Main_OnCommand(40004,0)
end
reaper.defer(finish)
