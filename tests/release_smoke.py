"""Verify the distributed ZIP itself using isolated profiles and real guest code.

Requires original assets locally. Never modifies them, deletes previous results,
or terminates unrelated processes. This does not certify public internet play.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--package', type=Path, default=ROOT / 'dist/Nidhogg-Windows-v1.0.0-x64.zip')
    parser.add_argument('--assets', type=Path, default=Path(os.environ.get('NIDHOGG_ASSETS', ROOT / 'CUSA00920')))
    args = parser.parse_args()
    assets = args.assets.resolve()
    assert (assets / 'eboot.bin').is_file() and (assets / 'Data').is_dir(), 'Supply --assets CUSA00920'
    original_hash = hashlib.sha256((assets / 'eboot.bin').read_bytes()).hexdigest()
    output = ROOT / 'tests/results' / ('release-v1.0.0-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    extracted = output / 'package'
    extracted.mkdir(parents=True)
    expected = {'Nidhogg.exe', 'Nidhogg.png', 'Play Online.cmd', 'READ-ME.txt',
                'READ-ME-AR.txt', 'RELEASE-NOTES.md', 'RELEASE-VERIFICATION.md', 'release-manifest.json',
                'MULTIPLAYER.md', 'NETWORK-VERIFICATION.md', 'THIRD_PARTY_NOTICES.md',
                'LibAtrac9-LICENSE.txt'}
    with zipfile.ZipFile(args.package) as z:
        assert set(z.namelist()) == expected, 'Unexpected package entries'
        assert z.testzip() is None, 'Corrupt ZIP'
        z.extractall(extracted)  # names above are an exact flat allowlist
    exe = extracted / 'Nidhogg.exe'
    manifest = json.loads((extracted / 'release-manifest.json').read_text(encoding='utf-8-sig'))
    exe_hash = hashlib.sha256(exe.read_bytes()).hexdigest()
    assert manifest['executableSha256'] == exe_hash
    checks = {'exact_asset_free_manifest': True, 'executable_matches_manifest': True}
    results = {}

    def run(label, sequence='', second='', frames=700, profile=None, mute=False, verify=False):
        user = output / (profile or label)
        (user / 'config').mkdir(parents=True, exist_ok=True)
        (user / 'config/settings.json').write_text(json.dumps({
            'vsync': 0, 'displayWidth': 640, 'displayHeight': 360, 'renderScale': .5,
            'mute': mute, 'launchDirectly': False}), encoding='utf8')
        shot = max(1, frames - 30)
        env = {k: v for k, v in os.environ.items() if not k.startswith('NIDHOGG_')}
        env.update(NIDHOGG_QA_UNFOCUSED='1', NIDHOGG_UNCAPPED='1',
                   NIDHOGG_AUTOPRESS=sequence, NIDHOGG_AUTOPRESS_P2=second,
                   NIDHOGG_SCREENSHOT=str(shot), NIDHOGG_SCREENSHOT_DIR=str(user / 'shots'))
        command = [str(exe), '--no-prompt', '--assets', str(assets), '--user-dir', str(user)]
        command += ['--verify-assets'] if verify else ['--max-frames', str(frames)]
        with (user / (label + '.stdout.log')).open('w') as stream:
            result = subprocess.run(command, cwd=extracted, env=env, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=150,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        checks[label + '_exit'] = result.returncode == 0
        if verify:
            return ''
        log = (user / 'logs/nidhogg.log').read_text(errors='replace')
        checks[label + '_screenshot'] = (user / f'shots/shot_{shot}.png').is_file()
        checks[label + '_renderer'] = '0 errors | [audio]' in log
        results[label] = {'exit': result.returncode,
                          'rooms': re.findall(r'Created room (\S+)', log),
                          'winners': re.findall(r'(\w+Player) wins match!', log)}
        return log

    run('asset-verification', verify=True)
    menu = run('save-create', '100:0x4000:8,280:0x4000:8,330:0x0040:6,360:0x0040:6,390:0x4000:8,430:0x0080:14,470:0x2000:8', frames=560, profile='save-profile')
    prefs = output / 'save-profile/saves/SAVEDATA00/prefs.bin'
    checks['save_created'] = prefs.is_file() and prefs.stat().st_size == 16
    before = prefs.read_bytes() if prefs.exists() else b''
    run('save-reload', '100:0x4000:8', frames=400, profile='save-profile')
    checks['save_preserved_after_restart'] = bool(before) and prefs.read_bytes() == before

    base = '100:0x4000:8,280:0x4000:8,330:0x4000:8,380:0x4000:8'
    for name, nav, room, start in [
        ('Castle', '', 'rCastleCenter', 440),
        ('Wilds', ',430:0x0040:8', 'rWildsCenter', 485),
        ('Mines', ',430:0x0040:8,475:0x0040:8', 'rMinesCenter', 530),
        ('Clouds', ',430:0x0010:8', 'rSkyCenter', 485),
    ]:
        sequence = base + nav + f',{start}:0x4000:8,{start+80}:0x8000:10,{start+100}:0x0020:60'
        log = run(name, sequence, f'{start+90}:0x0080:40,{start+105}:0x8000:8', start+170)
        checks[name + '_stage'] = 'Created room ' + room in log
        checks[name + '_audio'] = 'mixed=' in log and re.search(r'peak=0\.[0]*[1-9]', log) is not None

    sequence = base + ',440:0x4000:8,480:0x0040:35,520:0x0010:20,530:0x8000:8,555:0x0020:2200'
    sequence += ''.join(f',{f}:0x4000:12' for f in range(640, 2650, 95))
    sequence += ''.join(f',{f}:0x4000:8' for f in range(2750, 3950, 50))
    log = run('local-match-rematch', sequence, '480:0x0010:20,490:0x8000:8,575:0x0080:2200', 6500)
    checks['local_match_winner'] = 'yellowPlayer wins match!' in log
    checks['local_match_results'] = 'Created room rMatchStats' in log
    checks['local_rematch'] = log.count('Created room rCastleCenter') >= 2

    sequence = '100:0x4000:8,280:0x4000:8,330:0x0010:8,370:0x4000:8,470:0x4000:8'
    for f in range(540, 3400, 36):
        sequence += f',{f}:0x8000:6,{f+8}:0x0020:26'
        if f % 108 == 0: sequence += f',{f+14}:0x4000:10'
        if f % 144 == 0: sequence += f',{f+4}:0x0010:6'
    log = run('arcade', sequence, frames=3500)
    checks['arcade_progress'] = 'Created room rArcadeProgress' in log
    checks['arcade_gameplay'] = re.search(r'Created room r(Castle|Mines|Sky|Wilds)Center', log) is not None
    log = run('mute', base + ',440:0x4000:8,510:0x8000:8', mute=True)
    peaks = re.findall(r'peak=([0-9.]+)', log)
    checks['mute_zero_peak'] = bool(peaks) and all(float(value) == 0 for value in peaks)
    checks['original_eboot_unchanged'] = hashlib.sha256((assets / 'eboot.bin').read_bytes()).hexdigest() == original_hash
    report = {'tested_at': datetime.now(timezone.utc).isoformat(), 'scope': 'one physical Windows machine; packaged GUI binary; original game assets supplied locally',
              'executable_sha256': exe_hash, 'source_commit': manifest['sourceCommit'],
              'checks': checks, 'games': results, 'passed': all(checks.values())}
    (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf8')
    latest = ROOT / 'tests/results/release-v1.0.0-latest.json'
    latest.write_text(json.dumps({'report': str(output / 'report.json')}, indent=2), encoding='utf8')
    print(json.dumps({'report': str(output / 'report.json'), 'passed': sum(checks.values()),
                      'total': len(checks), 'failures': [k for k,v in checks.items() if not v]}, indent=2))
    raise SystemExit(0 if all(checks.values()) else 1)


if __name__ == '__main__':
    main()
