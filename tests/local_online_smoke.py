"""Own a localhost relay and two clients; never contacts the public service."""
import argparse,os,socket,subprocess,sys,time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/results/ui-performance-2026-11-13'

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--label',default='online-relay-final')
    parser.add_argument('--mixed-effects',action='store_true')
    parser.add_argument('--stage',type=int,choices=range(4),default=0)
    parser.add_argument('--frames',type=int,default=2800)
    parser.add_argument('--no-combat',action='store_true')
    parser.add_argument('--legacy-lobby',action='store_true')
    args=parser.parse_args()
    if not args.label.replace('-','').replace('_','').isalnum():parser.error('simple label required')
    OUT.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,PORT='29881',UDP_PORT='29882',BIND='127.0.0.1',RELAY_PUBLIC_IP='127.0.0.1')
    env['NIDHOGG_QA_LEGACY_LOBBY']='1' if args.legacy_lobby else '0'
    with (OUT/'local-relay.log').open('w') as log:
        server=subprocess.Popen(['node','server/server.mjs'],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline=time.monotonic()+10
            while True:
                if server.poll() is not None:raise RuntimeError('Owned relay exited; inspect local-relay.log')
                try:
                    with socket.create_connection(('127.0.0.1',29881),timeout=.2):break
                except OSError:
                    if time.monotonic()>deadline:raise
                    time.sleep(.1)
            return subprocess.run([sys.executable,'tests/multiplayer_pair.py','--endpoint','http://127.0.0.1:29881',
                '--assets',r'C:\Users\puy12\Downloads\g\CUSA00920','--frames',str(args.frames),'--stage',str(args.stage),
                '--label','ui-performance-2026-11-13/'+args.label]+([] if args.no_combat else ['--combat'])+(['--mixed-effects'] if args.mixed_effects else []),cwd=ROOT).returncode
        finally:
            server.terminate()
            try:server.wait(timeout=5)
            except subprocess.TimeoutExpired:server.kill();server.wait()

if __name__=='__main__':raise SystemExit(main())
