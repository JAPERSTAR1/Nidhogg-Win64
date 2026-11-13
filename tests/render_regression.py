"""Owned offline transition/victory run, isolated settings/saves and evidence."""
import argparse,csv,json,os,re,shutil,statistics,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--name',required=True);p.add_argument('--exe',default='port/out/nidhogg_host.exe')
    p.add_argument('--frames',type=int,default=3000);p.add_argument('--fps',type=int,default=240)
    p.add_argument('--uncapped',action='store_true');p.add_argument('--menu',action='store_true')
    p.add_argument('--shots',default='300,420,600,900,1200,1500,1800,2100,2400,2700,2900')
    p.add_argument('--integrated',action='store_true');p.add_argument('--inputs')
    p.add_argument('--stage',type=int,choices=range(4),default=0)
    p.add_argument('--settings',action='store_true')
    p.add_argument('--debug-d3d',action='store_true')
    p.add_argument('--original-effects',action='store_true')
    p.add_argument('--assets',default=r'C:\Users\puy12\Downloads\g\CUSA00920');a=p.parse_args()
    if not re.fullmatch('[a-zA-Z0-9_-]+',a.name):p.error('simple name required')
    out=ROOT/'tests/results/ui-performance-2026-11-13'/a.name
    if out.exists():p.error('case already exists')
    (out/'config').mkdir(parents=True)
    (out/'config/settings.json').write_text(json.dumps(dict(vsync=0,displayWidth=1280,displayHeight=720,
       renderScale=1,presentationFpsLimit=a.fps,showNetStats=False,showFps=False,
       showRoomTransitions=a.original_effects,showScreenFlashes=a.original_effects)))
    exe=out/'nidhogg_host.exe';shutil.copy2(ROOT/a.exe,exe)
    seq='100:0x4000:8,280:0x4000:8,330:0x4000:8,380:0x4000:8,440:0x4000:8,480:0x0040:35,520:0x0010:20,530:0x8000:8,555:0x0020:2200'
    seq+= ''.join(f',{f}:0x4000:12' for f in range(640,2650,95))
    if a.menu:seq='100:0x4000:8'
    # Native Multiplayer chooses Local Versus, then forwards the two original
    # game confirmations. Leave enough time before the level-select confirm.
    if a.integrated and not a.menu and not a.inputs:
        seq=','.join(f'{int(f)+ (30 if int(f)>=440 else 0)}:{m}:{d}' for f,m,d in (x.split(':') for x in seq.split(',')))
    if a.stage and not a.inputs:
        start=470 if a.integrated else 440
        moves=2 if a.stage==2 else 1
        seq=','.join(f'{int(f)+(45*moves if int(f)>=start else 0)}:{m}:{d}' for f,m,d in (x.split(':') for x in seq.split(',')))
        seq+=''.join(f',{start-10+i*45}:{"0x0010" if a.stage==3 else "0x0040"}:8' for i in range(moves))
    if a.inputs:seq=a.inputs
    env={k:v for k,v in os.environ.items() if not k.startswith('NIDHOGG_')}
    env.update(NIDHOGG_QA_UNFOCUSED='1',NIDHOGG_RENDER_PROFILE='1',NIDHOGG_AUTOPRESS=seq,
      NIDHOGG_AUTOPRESS_P2='' if a.menu else ('510:0x0010:20,520:0x8000:8,605:0x0080:2200' if a.integrated and not a.inputs else '480:0x0010:20,490:0x8000:8,575:0x0080:2200'),
      NIDHOGG_SCREENSHOT_DIR=str(out/'shots'),NIDHOGG_SCREENSHOT=a.shots)
    if a.stage and not a.inputs:
        env['NIDHOGG_AUTOPRESS_P2']=','.join(f'{int(f)+45*moves}:{m}:{d}' for f,m,d in (x.split(':') for x in env['NIDHOGG_AUTOPRESS_P2'].split(',')))
    if a.uncapped:env['NIDHOGG_UNCAPPED']='1'
    if not a.integrated:env['NIDHOGG_LEGACY_MENU_TEST']='1'
    if a.settings:env['NIDHOGG_OPEN_SETTINGS']='1'
    if a.debug_d3d:env['NIDHOGG_D3D_DEBUG']='1'
    start=time.monotonic()
    with (out/'stdout.log').open('w') as log:
        child=subprocess.Popen([str(exe),'--no-prompt','--assets',a.assets,'--user-dir',str(out),'--max-frames',str(a.frames)],
             env=env,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        try:code=child.wait(timeout=180)
        except subprocess.TimeoutExpired:child.kill();child.wait();raise
    raw=(out/'logs/nidhogg.log').read_text(errors='replace')
    report=dict(exit=code,seconds=round(time.monotonic()-start,2),rooms=re.findall('Created room (\\S+)',raw),
      winner=re.findall('(yellowPlayer|orangePlayer) wins match!',raw),fault='[fault]' in raw or 'Fatal Exception Report' in raw,
      screenshots=[x.name for x in (out/'shots').glob('*.png')])
    rows=list(csv.DictReader((out/'logs/render.csv').open()))
    report['renderMs']={}
    for room in sorted(set(x['room'] for x in rows)):
        values=sorted(float(x['render_ms']) for x in rows if x['room']==room)
        report['renderMs'][room]=dict(median=round(statistics.median(values),3),p95=values[int((len(values)-1)*.95)],maximum=max(values))
    (out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    return code
if __name__=='__main__':raise SystemExit(main())
