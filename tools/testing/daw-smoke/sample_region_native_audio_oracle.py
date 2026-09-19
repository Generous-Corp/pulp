#!/usr/bin/env python3
"""PUB-04 audio evidence helper using Pulp's canonical CLI compare plus scalar oracle."""
from __future__ import annotations
import argparse, hashlib, json, math, struct, subprocess, wave, tempfile
from pathlib import Path

def scalar_impulse(n=16384,a=.5):
    x=[0.0]*n; y=[0.0]*n; x[0]=1.0
    for i in range(n): y[i]=a*x[i]+(x[i-1] if i else 0.0)-a*(y[i-1] if i else 0.0)
    return y

def read_wav(path):
    with wave.open(str(path),'rb') as w:
        if w.getnchannels()!=1 or w.getsampwidth()!=4 or w.getframerate()!=48000: raise ValueError('expected mono float32 48k WAV')
        raw=w.readframes(w.getnframes())
    return list(struct.unpack('<%df'%(len(raw)//4),raw))

def main(argv=None):
    p=argparse.ArgumentParser(); p.add_argument('--reference',required=True); p.add_argument('--candidate',required=True); p.add_argument('--pulp',default='pulp'); p.add_argument('--evidence',required=True); p.add_argument('--tolerance',type=float,default=1e-6); a=p.parse_args(argv)
    ref=read_wav(a.reference); cand=read_wav(a.candidate); expected=scalar_impulse(len(cand)); err=max((abs(x-y) for x,y in zip(cand,expected)),default=math.inf)
    cmd=[a.pulp,'audio','validate','compare',a.reference,a.candidate,'--mode','null','--tolerance','-120']; proc=subprocess.run(cmd,text=True,capture_output=True,check=False)
    with tempfile.TemporaryDirectory(prefix='pulp-f4-negative-') as td:
        mutated=Path(td)/'mutated.wav'; data=list(cand); data[17 if len(data)>17 else 0]+=0.01
        with wave.open(str(mutated),'wb') as w:
            w.setnchannels(1); w.setsampwidth(4); w.setframerate(48000); w.writeframes(struct.pack('<%df'%len(data),*data))
        neg=subprocess.run([a.pulp,'audio','validate','compare',a.reference,str(mutated),'--mode','null','--tolerance','-180'],text=True,capture_output=True,check=False)
        negative_failed=(neg.returncode!=0)
    ev={'reference':str(Path(a.reference).resolve()),'candidate':str(Path(a.candidate).resolve()),'reference_sha256':hashlib.sha256(Path(a.reference).read_bytes()).hexdigest(),'candidate_sha256':hashlib.sha256(Path(a.candidate).read_bytes()).hexdigest(),'scalar_max_error':err,'scalar_pass':err<=a.tolerance,'pulp_compare_command':cmd,'pulp_compare_returncode':proc.returncode,'pulp_compare_stdout':proc.stdout,'pulp_compare_stderr':proc.stderr,'negative_control_required':True,'negative_control_returncode':neg.returncode,'negative_control_failed_as_expected':negative_failed,'status':'passed' if err<=a.tolerance and proc.returncode==0 and negative_failed else 'failed'}
    Path(a.evidence).write_text(json.dumps(ev,indent=2)+'\n'); return 0 if ev['status']=='passed' else 1
if __name__=='__main__': raise SystemExit(main())
