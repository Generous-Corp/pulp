#!/usr/bin/env python3
"""Run the real REAPER PUB-04 journey and validate its host-bound receipt."""
from __future__ import annotations
import argparse, hashlib, json, os, shutil, subprocess, tempfile, time
from pathlib import Path
FORMATS=('au','vst3','clap')
ROOT=Path(__file__).resolve().parent
LUA=ROOT/'sample_region_native_reaper.lua'

def tree_sha(path):
    h=hashlib.sha256()
    for child in sorted(path.rglob('*')):
        if child.is_file() and not child.is_symlink(): h.update(child.relative_to(path).as_posix().encode()+b'\0'+child.read_bytes())
    return h.hexdigest()

def run(fmt,bundle,out,timeout):
    out.mkdir(parents=True,exist_ok=True); receipt=out/f'{fmt}-receipt.log'; wav=out/f'{fmt}-impulse-output.wav'; project=out/f'{fmt}.rpp'
    state_before=out/f'{fmt}-state-before.bin'; state_after=out/f'{fmt}-state-after.bin'; input_wav=out/f'{fmt}-impulse-input.wav'
    import wave, struct
    with wave.open(str(input_wav),'wb') as w:
        w.setnchannels(1); w.setsampwidth(4); w.setframerate(48000); samples=[1.0]+[0.0]*16383; w.writeframes(struct.pack('<16384f',*samples))
    env=os.environ.copy(); env.update(PULP_F4_FORMAT=fmt,PULP_F4_FX_NAME='Sample Region Allpass',PULP_F4_PLUGIN_PATH=str(bundle),PULP_F4_WAV=str(wav),PULP_F4_PROJECT=str(project),PULP_F4_RECEIPT=str(receipt),PULP_F4_STATE_BEFORE=str(state_before),PULP_F4_STATE_AFTER=str(state_after),PULP_F4_INPUT_WAV=str(input_wav))
    reaper=env.get('REAPER_BIN','/Applications/REAPER.app/Contents/MacOS/REAPER')
    if not Path(reaper).is_file(): return {'format':fmt,'status':'inconclusive','reason':'REAPER unavailable'}
    cmd=[reaper,'-new','-nosplash','-script',str(LUA)]
    try: p=subprocess.run(cmd,env=env,text=True,capture_output=True,timeout=timeout,check=False)
    except subprocess.TimeoutExpired: return {'format':fmt,'status':'inconclusive','reason':'REAPER timeout'}
    lines=[x[len('[sample-region-f4] '):] for x in (p.stdout+'\n'+p.stderr).splitlines() if x.startswith('[sample-region-f4] ')]
    if not lines: return {'format':fmt,'status':'inconclusive','reason':'no real-host receipt','returncode':p.returncode}
    rec=json.loads(lines[-1]); rec['driver_returncode']=p.returncode
    rec['state_before_sha256']=hashlib.sha256(state_before.read_bytes()).hexdigest() if state_before.exists() else None
    rec['state_after_sha256']=hashlib.sha256(state_after.read_bytes()).hexdigest() if state_after.exists() else None
    rec['state_hash_equal']=rec['state_before_sha256'] is not None and rec['state_before_sha256']==rec['state_after_sha256']
    rec['wav_exists']=wav.exists(); rec['wav_sha256']=hashlib.sha256(wav.read_bytes()).hexdigest() if wav.exists() else None
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
        else: rec['audio_oracle_pass']=False
        results.append(rec)
    summary={'packet':'PKT-F4-01','acceptance':'PUB-04','results':results,'status':'passed' if all(r.get('status')=='passed' for r in results) else 'inconclusive'}
    a.out.mkdir(parents=True,exist_ok=True); (a.out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n'); print(json.dumps(summary,indent=2)); return 0 if summary['status']=='passed' else 3
if __name__=='__main__': raise SystemExit(main())
