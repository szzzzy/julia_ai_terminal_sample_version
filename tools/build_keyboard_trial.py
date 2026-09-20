"""Prepare/build an isolated keyboard endpoint experiment; never flash.

Private sdkconfig is copied into the ignored build directory, never printed.
Root sdkconfig and default build directory are left unchanged.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]

def overlay(source,settings):
    lines=[]
    for line in source.splitlines():
        match=re.match(r'(CONFIG_\w+)=|# (CONFIG_\w+) is not set$',line)
        if match and (match.group(1) or match.group(2)) in settings: continue
        lines.append(line)
    return '\n'.join(lines)+ '\n' + '\n'.join(f'{k}={v}' for k,v in settings.items())+'\n'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',action='store_true',help='Run idf.py build only, no flash')
    args=parser.parse_args()
    original=(ROOT/'sdkconfig').read_bytes()
    settings={}
    for line in (ROOT/'sdkconfig.keyboard-trial.defaults').read_text(encoding='utf-8').splitlines():
        if line.startswith('CONFIG_'):
            key,value=line.split('=',1);settings[key]=value
    dest=ROOT/'build-keyboard-trial';dest.mkdir(exist_ok=True)
    cfg=dest/'sdkconfig'
    cfg.write_text(overlay(original.decode('utf-8-sig'),settings),encoding='utf-8')
    manifest=dict(experimental=True,parameters=settings,source_sdkconfig_sha256=hashlib.sha256(original).hexdigest())
    try:
        if args.build:
            idf=os.environ.get('IDF_PATH')
            if not idf: raise RuntimeError('Activate ESP-IDF or set IDF_PATH before --build')
            subprocess.run([sys.executable,str(Path(idf)/'tools/idf.py'),'-B',str(dest),
                            '-D','SDKCONFIG='+str(cfg),'build'],cwd=ROOT,check=True)
            manifest['binary_sha256']=hashlib.sha256((dest/'julia_fused_base.bin').read_bytes()).hexdigest()
            # Verify Kconfig did not discard any requested experimental setting.
            final=cfg.read_text(encoding='utf-8')
            for key,value in settings.items():
                if f'{key}={value}' not in final.splitlines():
                    raise RuntimeError(f'Experimental setting not applied: {key}')
    finally:
        if (ROOT/'sdkconfig').read_bytes()!=original:
            raise RuntimeError('Root sdkconfig changed during experiment; inspect manually, not restored')
        (dest/'trial-manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print(f'Experimental config: {cfg}\nRoot sdkconfig unchanged. No flashing performed.')

if __name__=='__main__':main()
