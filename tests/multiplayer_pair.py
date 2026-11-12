"""Two real Windows guest processes; writes logs/screenshots, never touches original assets.
This is a localhost transport/simulation test, not proof of internet play.
"""
import argparse
import json
import os
import re
import shutil
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--assets', default=os.environ.get('NIDHOGG_ASSETS', str(ROOT / 'CUSA00920')))
    p.add_argument('--frames', type=int, default=1500)
    p.add_argument('--latency', type=int, default=0, help='UDP simulator RTT (direct transport only)')
    p.add_argument('--direct', action='store_true')
    p.add_argument('--wss', action='store_true', help='Force TCP/WebSocket relay for comparison')
    p.add_argument('--fixed-delay', action='store_true', help='Disable automatic pre-match delay calibration')
    p.add_argument('--loss', type=int, default=0, help='Controlled outgoing UDP loss percent per client')
    p.add_argument('--jitter', type=int, default=0, help='Controlled outgoing UDP jitter in ms per client')
    p.add_argument('--timeout', type=int, default=0, help='Wall-clock test deadline override')
    p.add_argument('--delay', type=int, default=3)
    p.add_argument('--endpoint', default='http://127.0.0.1:27880')
    p.add_argument('--label', default='internet-local')
    p.add_argument('--combat', action='store_true')
    p.add_argument('--disconnect-frame', type=int, default=0)
    args = p.parse_args()
    result = ROOT / 'tests' / 'results' / args.label
    result.mkdir(parents=True, exist_ok=True)
    clients, files = [], []
    code = result / 'room-code.txt'
    code.unlink(missing_ok=True)
    try:
        for role in ['host', 'guest']:
            user = result / role
            (user / 'config').mkdir(parents=True, exist_ok=True)
            (user / 'config' / 'settings.json').write_text(json.dumps({
                'vsync': 0, 'displayWidth': 640, 'displayHeight': 360,
                'renderScale': .5, 'showNetStats': True, 'lobbyServerUrl': args.endpoint,
                'playerName': role, 'inputDelayFrames': args.delay,
                'autoInputDelay': not args.fixed_delay,
            }))
            env = {k: v for k, v in os.environ.items() if not k.startswith('NIDHOGG_')}
            env.update(NIDHOGG_QA_UNFOCUSED='1', NIDHOGG_FORCE_RELAY='0' if args.direct else '1',
                NIDHOGG_FORCE_WSS='1' if args.wss else '0',
                NIDHOGG_NET_SIM_JITTER_MS=str(args.jitter), NIDHOGG_NET_SIM_LOSS_PCT=str(args.loss),
                NIDHOGG_NET_DUMP='1',
                NIDHOGG_NET_AUTO_EXIT='1',
                NIDHOGG_NET_QA_COMBAT='1' if args.combat else '0',
                NIDHOGG_NET_AUTO='host_private' if role == 'host' else 'join_code',
                NIDHOGG_NET_CODE_OUT=str(code), NIDHOGG_SCREENSHOT_DIR=str(user / 'shots'),
                NIDHOGG_SCREENSHOT='600,1000,1400', NIDHOGG_NET_SIM_LATENCY_MS=str(args.latency))
            if role == 'guest':
                deadline = time.monotonic() + 20
                while (not code.exists() or len(code.read_text().strip()) != 6) and time.monotonic() < deadline:
                    if clients[0].poll() is not None: raise RuntimeError('Host exited before creating room')
                    time.sleep(.1)
                if not code.exists() or len(code.read_text().strip()) != 6:
                    raise RuntimeError('Host could not create a room; inspect its network log')
                env['NIDHOGG_NET_ARG'] = code.read_text().strip()
            executable = user / 'nidhogg_host.exe'
            shutil.copy2(ROOT / 'port/out/nidhogg_host.exe', executable)
            output = open(user / 'stdout.log', 'w', encoding='utf8'); files.append(output)
            clients.append(subprocess.Popen([str(executable),
                '--no-prompt', '--assets', args.assets, '--user-dir', str(user),
                '--max-frames', str(args.disconnect_frame if role == 'guest' and args.disconnect_frame else args.frames)], env=env, stdout=output, stderr=subprocess.STDOUT,
                cwd=ROOT, creationflags=subprocess.CREATE_NO_WINDOW))
        deadline = time.monotonic() + (args.timeout or max(80, args.frames / 60 * 3))
        while any(c.poll() is None for c in clients) and time.monotonic() < deadline: time.sleep(.2)
        report = {}
        for role, client in zip(['host', 'guest'], clients):
            log = (result / role / 'logs/nidhogg.log').read_text(errors='replace')
            report[role] = {'exit': client.poll(), 'barrier': 'BARRIER SYNCED' in log,
                'desync': 'DESYNC' in log, 'timeout': 'timed out' in log,
                'wins': re.findall(r'(yellowPlayer|orangePlayer) wins match!', log),
                'hashes': dict(re.findall(r'frame=(\d+).*?hash=([0-9a-f]+) sync=OK', log)),
                'rooms': [x for x in log.splitlines() if 'Created room' in x],
                'stopped': 'match stopped:' in log,
                'metrics': re.findall(r'\[netplay-metrics\] (.*)', log),
                'agreedDelay': re.findall(r'agreed delay=(\d+)f', log),
                'network': [x for x in log.splitlines() if '[netplay' in x][-30:]}
        (result / 'report.json').write_text(json.dumps(report, indent=2))
        print(json.dumps({role: {k: v for k, v in r.items() if k not in ('network', 'hashes')} | {'hashChecks': len(r['hashes'])} for role,r in report.items()}, indent=2))
        a, b = report['host'], report['guest']
        common = set(a['hashes']) & set(b['hashes'])
        passed = all(r['exit'] == 0 and r['barrier'] and not r['desync'] for r in report.values()) and len(common) >= 3 and all(a['hashes'][f] == b['hashes'][f] for f in common)
        if args.disconnect_frame: passed = passed and a['stopped']
        if args.combat: passed = passed and bool(a['wins']) and a['wins'] == b['wins']
        if not passed: raise SystemExit(1)
    finally:
        for c in clients:
            if c.poll() is None:
                subprocess.run(['taskkill.exe', '/PID', str(c.pid), '/T', '/F'], capture_output=True)
                c.wait(timeout=10)
        for f in files: f.close()

if __name__ == '__main__': main()
