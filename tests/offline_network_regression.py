"""Bounded offline regressions with isolated saves; no process-wide kill/delete."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ASSETS = os.environ.get('NIDHOGG_ASSETS', str(ROOT / 'CUSA00920'))
OUT = ROOT / 'tests/results' / os.environ.get('NIDHOGG_TEST_LABEL','offline-network-regression')
OUT.mkdir(parents=True, exist_ok=True)
checks = {}
for file, expected in [('nidhogg_host.exe', 3), ('Nidhogg.exe', 2)]:
    data = (ROOT / 'port/out' / file).read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    checks[file + ' subsystem'] = struct.unpack_from('<H', data, pe + 24 + 68)[0] == expected

def run(label, sequence, second='', frames=800):
    user = OUT / label
    (user / 'config').mkdir(parents=True, exist_ok=True)
    (user / 'config/settings.json').write_text(json.dumps({'vsync': 0, 'presentationFpsLimit': 0, 'displayWidth': 640, 'displayHeight': 360, 'renderScale': .5}))
    env = {k: v for k, v in os.environ.items() if not k.startswith('NIDHOGG_')}
    env.update(NIDHOGG_QA_UNFOCUSED='1', NIDHOGG_UNCAPPED='1', NIDHOGG_AUTOPRESS=sequence,
        NIDHOGG_AUTOPRESS_P2=second, NIDHOGG_SCREENSHOT_DIR=str(user / 'shots'), NIDHOGG_SCREENSHOT='600')
    executable = user / 'Nidhogg.exe'; shutil.copy2(ROOT / 'port/out/Nidhogg.exe', executable)
    with (user / 'stdout.log').open('w') as output:
        result = subprocess.run([str(executable), '--no-prompt', '--assets', ASSETS, '--user-dir', str(user), '--max-frames', str(frames)],
            env=env, stdout=output, stderr=subprocess.STDOUT, timeout=90, creationflags=subprocess.CREATE_NO_WINDOW)
    log = (user / 'logs/nidhogg.log').read_text(errors='replace')
    checks[label + ' clean exit'] = result.returncode == 0
    checks[label + ' renderer'] = '0 errors | [audio]' in log
    checks[label + ' audio mixed'] = 'voices=0 peak=0.0000' not in log and 'mixed=' in log
    checks[label + ' screenshot'] = (user / 'shots/shot_600.png').exists()
    return log

sequence = '100:0x4000:8,280:0x4000:8,330:0x4000:8,380:0x4000:8,440:0x4000:8,480:0x0040:35,520:0x0010:20,530:0x8000:8,555:0x0020:2200'
for frame in range(640, 2650, 95): sequence += f',{frame}:0x4000:12'
# Multiplayer now opens the native Local/Online choice before routing into
# the original level selector. Keep later gameplay inputs clear of that route.
sequence=','.join(f'{int(f)+(30 if int(f)>=440 else 0)}:{m}:{d}' for f,m,d in (x.split(':') for x in sequence.split(',')))
log = run('local-versus', sequence, '510:0x0010:20,520:0x8000:8,605:0x0080:2200', 2400)
checks['local versus completion'] = 'yellowPlayer wins match!' in log and 'Created room rMatchStats' in log
log = run('arcade', '100:0x4000:8,280:0x4000:8,330:0x0010:8,370:0x4000:8,470:0x4000:8,540:0x8000:8', frames=800)
checks['arcade entry'] = 'Created room rArcadeProgress' in log
(OUT / 'report.json').write_text(json.dumps(checks, indent=2))
print(json.dumps(checks, indent=2))
raise SystemExit(0 if all(checks.values()) else 1)
