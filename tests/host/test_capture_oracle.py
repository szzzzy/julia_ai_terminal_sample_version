"""Differential test against the actual inspected cloud NoiseFloorTracker class."""
import argparse
import ast
from collections import deque
import math
from pathlib import Path
import random
import subprocess
import time
import numpy as np

p=argparse.ArgumentParser()
p.add_argument('--cloud',type=Path,required=True)
p.add_argument('--driver',type=Path,required=True)
a=p.parse_args()
tree=ast.parse(a.cloud.read_text(encoding='utf-8'))
nodes=[n for n in tree.body if isinstance(n,(ast.ClassDef,ast.FunctionDef)) and n.name in ('NoiseFloorTracker','rms_dbfs')]
ns=dict(np=np,deque=deque,math=math,time=time)
exec(compile(ast.Module(body=nodes,type_ignores=[]),str(a.cloud),'exec'),ns)
# Scale the reference time unit to integer ms. This compares exact 20ms sampling
# rather than differences caused by the old Python wall-clock float boundary.
cfg=dict(fast_window_s=1500,slow_window_s=8000,update_interval_s=500,
         up_max_db_per_s=.003,down_max_db_per_s=.0005)
floor=ns['NoiseFloorTracker'](-60,cfg)
commands=[];expected=[]
def frame(t,db,frozen=False):
    floor.on_frame(db,frozen,ts=t);commands.append(f'F {t} {db} {int(frozen)}');expected.append(floor.bg())
def reset(bg):
    floor.reset(bg);commands.append(f'R {bg}');expected.append(bg)
rng=random.Random(715)
for t in range(0,24000,20):
    db=(-60+rng.uniform(-2,2) if t<3000 else -40+rng.uniform(-1,1) if t<12000 else -68+rng.uniform(-2,2))
    frame(t,db,6000<=t<9000)
reset(-60)
for t in range(0,10000,20):frame(t,-120 if t%80 else -40)
# feed_segment uses hard-coded 0.02s timestamps; compare its operations with the
# equivalent 20ms injection and the original _update twice at the same instant.
for number in range(6):
    now=20000+number*17000;n=750
    levels=[rng.uniform(-55,-45) for _ in range(n)]
    for i,d in enumerate(levels):floor.on_frame(d,False,ts=now-(n-i)*20)
    floor._update(now);floor._update(now)
    commands.append(f'S {now} {n} '+' '.join(map(str,levels)));expected.append(floor.bg())
reset(-60)
clock=0
for turn in range(20):
    for _ in range(150):
        clock+=20;frame(clock,rng.uniform(-64,-52))
    count=rng.randrange(20,151);clock+=count*20
    levels=[rng.uniform(-64,-52) for _ in range(count)]
    for i,d in enumerate(levels):floor.on_frame(d,False,ts=clock-(count-i)*20)
    floor._update(clock);floor._update(clock)
    commands.append(f'S {clock} {count} '+' '.join(map(str,levels)));expected.append(floor.bg())
# A delayed verdict can backfill behind newer idle frames. Preserve the cloud
# deque's ordering and both forced updates rather than silently truncating it.
reset(-60)
for t in range(0,8000,20):frame(t,rng.uniform(-66,-60))
for k in range(4):
    clock=8000+k*20;levels=[rng.uniform(-70,-60) for _ in range(750)]
    for i,d in enumerate(levels):floor.on_frame(d,False,ts=clock-(750-i)*20)
    floor._update(clock);floor._update(clock)
    commands.append(f'S {clock} 750 '+' '.join(map(str,levels)));expected.append(floor.bg())
for scale in (0,1,10,100,1000,32768):
    for _ in range(30):
        pcm=np.array([rng.randrange(-scale,min(scale+1,32768)) if scale else 0 for i in range(320)],dtype=np.int16)
        commands.append('P '+' '.join(map(str,pcm)));expected.append(ns['rms_dbfs'](pcm))
result=subprocess.run([str(a.driver.resolve())],input='\n'.join(commands)+'\n',text=True,capture_output=True,check=True)
actual=list(map(float,result.stdout.split()))
assert len(actual)==len(expected),(len(actual),len(expected))
error=max(abs(x-y) for x,y in zip(actual,expected))
for i,(x,y) in enumerate(zip(actual,expected)):
    assert abs(x-y)<2e-5,(i,commands[i][:100],x,y)
print(f'{len(actual)} differential operations passed; maximum dB error={error:.9g}')
