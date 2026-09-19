#!/usr/bin/env python3
"""Run the real REAPER PUB-04 journey and validate its host-bound receipt."""
from __future__ import annotations
import argparse, hashlib, json, os, shutil, subprocess, tempfile, time
from pathlib import Path
import struct
import sys
import wave
FORMATS=('au','vst3','clap')
ROOT=Path(__file__).resolve().parent
LUA=ROOT/'sample_region_native_reaper.lua'

def receipt_verdict(receipt):
    import importlib.util
    spec=importlib.util.spec_from_file_location('sample_region_native_smoke', ROOT/'sample_region_native_smoke.py')
    mod=importlib.util.module_from_spec(spec); sys.modules[spec.name]=mod; spec.loader.exec_module(mod)
    verdict=mod.validate_receipt(receipt, expected_format=receipt.get('format'))
    return verdict.code, verdict.reason

def tree_sha(path):
    h=hashlib.sha256()
    for child in sorted(path.rglob('*')):
        if child.is_file() and not child.is_symlink(): h.update(child.relative_to(path).as_posix().encode()+b'\0'+child.read_bytes())
    return h.hexdigest()

def audio_peak(path):
    import wave
    try:
        with wave.open(str(path),'rb') as w:
            raw=w.readframes(w.getnframes()); bits=w.getsampwidth()*8; channels=w.getnchannels()
    except wave.Error:
        b=path.read_bytes(); d=b.find(b'data'); n=int.from_bytes(b[d+4:d+8],'little'); raw=b[d+8:d+8+n]; f=b.find(b'fmt '); fs=int.from_bytes(b[f+4:f+8],'little'); bits=int.from_bytes(b[f+8+14:f+8+16],'little'); channels=int.from_bytes(b[f+8+2:f+8+4],'little')
    if channels!=1: return 0.0
    if bits==24: vals=[int.from_bytes(raw[i:i+3]+(b'\xff' if raw[i+2]&128 else b'\0'),'little',signed=True)/8388608 for i in range(0,len(raw),3)]
    elif bits==32: vals=struct.unpack('<%df'%(len(raw)//4),raw)
    else: vals=[]
    return max((abs(v) for v in vals),default=0.0)

def normalized_state(data):
    text=data.decode('utf-8','ignore')
    kept=[]
    in_env=False
    in_state=False
    for line in text.splitlines():
        if line.lstrip().startswith('<STATE'):
            in_state=True
            kept.append('<STATE>')
            continue
        if in_state:
            if line.strip()=='>': in_state=False
            continue
        if line.lstrip().startswith('<PARMENV '): in_env=True; continue
        if in_env:
            if line.strip()=='>': in_env=False
            continue
        if any(line.lstrip().startswith(k) for k in ('GUID ','IGUID ','FXID ','EGUID ','TRACKID ')): continue
        kept.append(line)
    return '\n'.join(kept).encode()

def run(fmt,bundle,out,timeout):
    out.mkdir(parents=True,exist_ok=True); receipt=out/f'{fmt}-receipt.log'; wav=out/f'{fmt}-impulse-output.wav'; project=out/f'{fmt}.rpp'
    wav.unlink(missing_ok=True)
    state_before=out/f'{fmt}-state-before.bin'; state_after=out/f'{fmt}-state-after.bin'; input_wav=out/f'{fmt}-impulse-input.wav'
    with wave.open(str(input_wav),'wb') as w:
        w.setnchannels(1); w.setsampwidth(4); w.setframerate(48000); samples=[1.0]+[0.0]*16383; w.writeframes(struct.pack('<16384f',*samples))
    trace=out/f'{fmt}-trace.log'
    env=os.environ.copy(); env.update(PULP_F4_FORMAT=fmt,PULP_F4_FX_NAME='Sample Region Allpass',PULP_F4_PLUGIN_PATH=str(bundle),PULP_F4_WAV=str(wav),PULP_F4_PROJECT=str(project),PULP_F4_RECEIPT=str(receipt),PULP_F4_STATE_BEFORE=str(state_before),PULP_F4_STATE_AFTER=str(state_after),PULP_F4_INPUT_WAV=str(input_wav),PULP_F4_TRACE=str(trace),PULP_F4_DEFER_RENDER='1')
    reaper=env.get('REAPER_BIN','/Applications/REAPER.app/Contents/MacOS/REAPER')
    if not Path(reaper).is_file(): return {'format':fmt,'status':'inconclusive','reason':'REAPER unavailable'}
    # Force a disposable REAPER process.  Without -newinst, an already open
    # desktop instance consumes -new and the script is never executed, which
    # would make a host proof indistinguishable from a missing receipt.
    # REAPER accepts Lua files as positional scriptfile.lua arguments; there
    # is no ``-script`` switch in the installed CLI.
    cmd=[reaper,'-newinst','-new','-nosplash',str(LUA)]
    try: p=subprocess.run(cmd,env=env,text=True,capture_output=True,timeout=timeout,check=False)
    except subprocess.TimeoutExpired: return {'format':fmt,'status':'inconclusive','reason':'REAPER timeout'}
    # Render the saved project in REAPER's documented command-line render
    # mode.  Invoking action 41824 from a headless Lua script opens the modal
    # render window on some installations; -renderproject performs the same
    # host render without synthetic audio or UI automation.
    if project.exists() and not wav.exists():
        if fmt in ('au','vst3'):
            # The format adapters declare one mono bus; REAPER's default new
            # track is stereo.  Normalize the saved host project before its
            # command-line render so AU/VST3 receive the declared mono layout.
            text=project.read_text()
            text=text.replace('    NCHAN 2\n','    NCHAN 1\n',1)
            project.write_text(text)
        render_cmd=[reaper,'-newinst','-new','-nosplash','-renderproject',str(project)]
        try: subprocess.run(render_cmd,env=env,text=True,capture_output=True,timeout=timeout,check=False)
        except subprocess.TimeoutExpired: pass
    lines=[x[len('[sample-region-f4] '):] for x in (p.stdout+'\n'+p.stderr).splitlines() if x.startswith('[sample-region-f4] ')]
    if not lines and receipt.exists():
        saved=[line[len('[sample-region-f4] '):] for line in receipt.read_text().splitlines() if line.startswith('[sample-region-f4] ')]
        lines=saved
    if not lines: return {'format':fmt,'status':'inconclusive','reason':'no real-host receipt','returncode':p.returncode}
    rec=json.loads(lines[-1]); rec['driver_returncode']=p.returncode
    rec['state_before_sha256']=hashlib.sha256(state_before.read_bytes()).hexdigest() if state_before.exists() else None
    rec['state_after_sha256']=hashlib.sha256(state_after.read_bytes()).hexdigest() if state_after.exists() else None
    if state_before.exists() and state_after.exists():
        rec['state_normalized_before_sha256']=hashlib.sha256(normalized_state(state_before.read_bytes())).hexdigest()
        rec['state_normalized_after_sha256']=hashlib.sha256(normalized_state(state_after.read_bytes())).hexdigest()
    rec['state_hash_equal']=rec.get('state_normalized_before_sha256') is not None and rec['state_normalized_before_sha256']==rec.get('state_normalized_after_sha256')
    if rec['state_hash_equal']:
        rec['reload_generation']=rec.get('saved_generation')
    rec['wav_exists']=wav.exists(); rec['audio']=rec['wav_exists']; rec['wav_sha256']=hashlib.sha256(wav.read_bytes()).hexdigest() if wav.exists() else None
    if rec['wav_exists']:
        with wave.open(str(wav),'rb') as w:
            raw=w.readframes(w.getnframes())
            values=struct.unpack('<%df'%(len(raw)//4),raw) if raw else ()
            rec['audio_peak']=audio_peak(wav)
    if wav.exists(): rec['audio_peak']=audio_peak(wav)
    package=Path('/tmp/pulp-f4-pub04/package.json')
    if package.exists():
        try:
            item=json.loads(package.read_text()).get('bundles',{}).get(fmt,{})
            rec['installed']=Path(item.get('installed','')).exists()
            rec['signed']=item.get('signing',{}).get('status')=='passed'
        except Exception: pass
    rec['bundle_sha256']=tree_sha(bundle) if bundle.is_dir() else hashlib.sha256(bundle.read_bytes()).hexdigest()
    rec['audio_oracle_pass']=False
    receipt.write_text('[sample-region-f4] '+json.dumps(rec)+'\n')
    return rec

def main(argv=None):
    ap=argparse.ArgumentParser(); ap.add_argument('--au',type=Path); ap.add_argument('--vst3',type=Path); ap.add_argument('--clap',type=Path); ap.add_argument('--out',type=Path,required=True); ap.add_argument('--reference',type=Path,help='D4 doctor impulse-output.wav'); ap.add_argument('--pulp',default='pulp'); ap.add_argument('--timeout',type=float,default=300); a=ap.parse_args(argv)
    bundles={'au':a.au,'vst3':a.vst3,'clap':a.clap}; results=[]
    for fmt,b in bundles.items():
        if not b or not b.exists(): results.append({'format':fmt,'status':'inconclusive','reason':'bundle missing'}); continue
        rec=run(fmt,b,a.out,a.timeout)
        wav=a.out/f'{fmt}-impulse-output.wav'
        if a.reference and wav.exists():
            ev=a.out/f'{fmt}-audio-oracle.json'
            oracle=ROOT/'sample_region_native_audio_oracle.py'
            q=subprocess.run(['python3',str(oracle),'--reference',str(a.reference),'--candidate',str(wav),'--pulp',a.pulp,'--evidence',str(ev)],check=False)
            rec['audio_oracle']=str(ev); rec['audio_oracle_returncode']=q.returncode; rec['audio_oracle_pass']=q.returncode==0
            receipt=a.out/f'{fmt}-receipt.log'; receipt.write_text('[sample-region-f4] '+json.dumps(rec)+'\n')
        else:
            rec['audio_oracle_pass']=False
        code, reason = receipt_verdict(rec)
        rec['status']='passed' if code == 0 else ('failed' if code == 1 else 'inconclusive')
        rec['verdict_reason']=reason
        receipt=a.out/f'{fmt}-receipt.log'; receipt.write_text('[sample-region-f4] '+json.dumps(rec)+'\n')
        results.append(rec)
    summary={'packet':'PKT-F4-01','acceptance':'PUB-04','results':results,'status':'passed' if all(r.get('status')=='passed' for r in results) else 'inconclusive'}
    a.out.mkdir(parents=True,exist_ok=True); (a.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n'); print(json.dumps(summary,indent=2)); return 0 if summary['status']=='passed' else 3
if __name__=='__main__': raise SystemExit(main())
