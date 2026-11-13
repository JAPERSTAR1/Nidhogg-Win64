"""Drive Multiplayer through imported game-pad input against an owned local lobby.

Checks public browsing, private creation, map selection, automatic guest admission,
and an in-process/window match start. Never uses the public service or modifies assets.
"""
import json
import argparse
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/results/integrated-front-end-2026-11-13/native-flow'
BASE = 'http://127.0.0.1:29881'


def api(path, body=None):
    request = Request(BASE + path, data=json.dumps(body).encode() if body is not None else None,
                      headers={'X-Nidhogg-Version': 'nidhogg-win64-2.1', 'Content-Type': 'application/json'})
    with urlopen(request, timeout=3) as response:
        return json.load(response)


def main():
    global OUT
    parser=argparse.ArgumentParser()
    parser.add_argument('--label',default='native-flow')
    args=parser.parse_args()
    if not re.fullmatch('[a-zA-Z0-9_-]+',args.label): parser.error('simple label required')
    OUT=OUT.parent/args.label
    if OUT.exists():
        raise RuntimeError('Evidence folder already exists; preserve it and choose another case name')
    OUT.mkdir(parents=True)
    processes, outputs = [], []
    env = dict(os.environ, PORT='29881', UDP_PORT='29882', BIND='127.0.0.1', RELAY_PUBLIC_IP='127.0.0.1')
    server_log = (OUT / 'server.log').open('w'); outputs.append(server_log)
    server = subprocess.Popen(['node', 'server/server.mjs'], cwd=ROOT, env=env, stdout=server_log,
                              stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    processes.append(server)
    try:
        deadline = time.monotonic() + 10
        while True:
            if server.poll() is not None:
                raise RuntimeError('Owned server exited')
            try:
                with socket.create_connection(('127.0.0.1', 29881), timeout=.2):
                    break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(.1)
        # A public entry must be visible; the second private entry must be hidden.
        public = api('/api/rooms/create', dict(roomName='CASTLE DUEL', hostName='RIVAL', stageIndex=0, isPublic=True))
        private = api('/api/rooms/create', dict(roomName='HIDDEN ROOM', hostName='PRIVATE', stageIndex=1, isPublic=False))
        clients = {}
        for role in ('host', 'guest'):
            user = OUT / role
            (user / 'config').mkdir(parents=True)
            (user / 'config/settings.json').write_text(json.dumps(dict(vsync=0, presentationFpsLimit=60,
                displayWidth=1280, displayHeight=720, renderScale=.5, lobbyServerUrl=BASE,
                playerName='HOST' if role=='host' else 'CHALLENGER', showFps=False, showNetStats=False)))
            client_env = {k:v for k,v in os.environ.items() if not k.startswith('NIDHOGG_')}
            client_env.update(NIDHOGG_QA_UNFOCUSED='1', NIDHOGG_FORCE_RELAY='1', NIDHOGG_NET_DUMP='1',
                NIDHOGG_NET_AUTO_EXIT='1', NIDHOGG_SCREENSHOT_DIR=str(user/'shots'),
                NIDHOGG_SCREENSHOT='350,850,1190,1450')
            if role == 'host':
                # Original Multiplayer -> hub -> private room -> change Castle to
                # Wilds in-room -> host Start. Neither client presses Ready.
                client_env['NIDHOGG_AUTOPRESS'] = (
                    '100:0x4000:8,280:0x4000:8,330:0x4000:8,'
                    '370:0x40:8,400:0x40:8,430:0x4000:8,'
                    '1000:0x40:8,1040:0x20:8,1080:0x20:8,1120:0x20:8,'
                    '1160:0x40:8,1200:0x40:8,1240:0x4000:8')
            else:
                host_log = OUT / 'host/logs/nidhogg.log'
                deadline = time.monotonic() + 35
                while True:
                    raw = host_log.read_text(errors='replace') if host_log.exists() else ''
                    match = re.search(r'created PRIVATE room id=\S+ code=([A-Z0-9]{6})', raw)
                    if match:
                        break
                    if clients['host'].poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('Native host action failed to create a private room')
                    # Keep the example entries alive until the hub was captured.
                    api('/api/rooms/heartbeat', {k:public[k] for k in ('roomId','authToken')})
                    time.sleep(.2)
                client_env.update(NIDHOGG_NET_AUTO='join_code', NIDHOGG_NET_ARG=match[1])
            exe = user / 'nidhogg_host.exe'
            shutil.copy2(ROOT / 'port/out/nidhogg_host.exe', exe)
            output = (user/'stdout.log').open('w'); outputs.append(output)
            child = subprocess.Popen([str(exe), '--no-prompt', '--assets', r'C:\Users\puy12\Downloads\g\CUSA00920',
                '--user-dir', str(user), '--max-frames', '1600'], cwd=ROOT, env=client_env,
                stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            processes.append(child); clients[role] = child
        deadline = time.monotonic() + 100
        while any(c.poll() is None for c in clients.values()) and time.monotonic() < deadline:
            for role in clients:
                for shot in (OUT/role/'shots').glob('*.png'):
                    preserved=OUT/role/('first-'+shot.name)
                    if not preserved.exists():
                        try: shutil.copy2(shot,preserved)
                        except PermissionError: pass
            time.sleep(.2)
        report = {}
        for role, child in clients.items():
            raw = (OUT/role/'logs/nidhogg.log').read_text(errors='replace')
            windows = re.findall(r'\[window\] created pid=\d+ hwnd=(\S+)',raw)
            resets = re.findall(r'\[guest-reset\] completed in process pid=\d+ hwnd=(\S+)',raw)
            report[role] = dict(exit=child.poll(), sameProcess=f'in-process deterministic boot pid={child.pid}' in raw,
                sameWindow=len(windows)==1 and bool(resets) and all(w==windows[0] for w in resets),
                wilds='Created room rWildsCenter' in raw, desync='DESYNC' in raw,
                hashes=dict(re.findall(r'frame=(\d+).*?hash=([0-9a-f]+) sync=OK',raw)))
        a,b = report.values()
        common = set(a['hashes']) & set(b['hashes'])
        passed = all(r['exit']==0 and r['sameProcess'] and r['sameWindow'] and r['wilds'] and not r['desync']
            for r in report.values()) and len(common)>=3 and all(a['hashes'][f]==b['hashes'][f] for f in common)
        report['passed'] = passed
        (OUT/'report.json').write_text(json.dumps(report,indent=2))
        print(json.dumps(report,indent=2))
        return 0 if passed else 1
    finally:
        for child in reversed(processes):
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=5)
                except subprocess.TimeoutExpired: child.kill(); child.wait()
        for output in outputs: output.close()


if __name__ == '__main__':
    raise SystemExit(main())
