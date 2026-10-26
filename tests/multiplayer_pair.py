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
    p.add_argument('--assets', default=r'C:\Users\puy12\Downloads\g\CUSA00920')
    p.add_argument('--frames', type=int, default=1500)
    p.add_argument('--latency', type=int, default=0, help='UDP simulator RTT (direct transport only)')
    p.add_argument('--direct', action='store_true')
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
            }))
            env = {k: v for k, v in os.environ.items() if not k.startswith('NIDHOGG_')}
            env.update(NIDHOGG_QA_UNFOCUSED='1', NIDHOGG_FORCE_RELAY='0' if args.direct else '1',
                NIDHOGG_NET_DUMP='1',
                NIDHOGG_NET_AUTO_EXIT='1',
                NIDHOGG_NET_QA_COMBAT='1' if args.combat else '0',
                NIDHOGG_NET_AUTO='host_private' if role == 'host' else 'join_code',
                NIDHOGG_NET_CODE_OUT=str(code), NIDHOGG_SCREENSHOT_DIR=str(user / 'shots'),
                NIDHOGG_SCREENSHOT='600,1000,1400', NIDHOGG_NET_SIM_LATENCY_MS=str(args.latency))
