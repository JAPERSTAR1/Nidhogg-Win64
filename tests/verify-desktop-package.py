"""Smoke-test the delivered portable EXE with no external asset path supplied."""
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
report = json.loads((root / 'tests/results/desktop-package-report.json').read_text())
folder = Path(report['windows_folder'])
output = root / 'tests/results/desktop-package-smoke'
output.mkdir(parents=True, exist_ok=True)
env = {key: value for key, value in os.environ.items() if not key.startswith('NIDHOGG_')}
env.update(NIDHOGG_QA_UNFOCUSED='1', NIDHOGG_UNCAPPED='1',
           NIDHOGG_AUTOPRESS='100:0x4000:8',
           NIDHOGG_SCREENSHOT='300', NIDHOGG_SCREENSHOT_DIR=str(output / 'shots'))
exe = str(folder / 'Nidhogg.exe')
args = [exe, '--no-prompt', '--user-dir', str(output / 'user')]
with (output / 'verify.stdout.log').open('w') as stream:
    verified = subprocess.run(args + ['--verify-assets'], cwd=root, env=env,
        stdout=stream, stderr=subprocess.STDOUT, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
assert verified.returncode == 0, 'Portable asset discovery failed'
with (output / 'game.stdout.log').open('w') as stream:
    played = subprocess.run(args + ['--max-frames', '330'], cwd=root, env=env,
        stdout=stream, stderr=subprocess.STDOUT, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
log = (output / 'user/logs/nidhogg.log').read_text(errors='replace')
checks = {
    'auto_discovered_bundled_assets': verified.returncode == 0,
    'loaded_bundled_original_executable': str(folder / 'CUSA00920/eboot.bin') in log,
    'clean_exit': played.returncode == 0,
    'reached_original_main_menu': 'Created room rMainMenu' in log,
    'rendered_without_gnm_errors': '0 errors | [audio]' in log,
    'saved_screenshot': (output / 'shots/shot_300.png').is_file(),
    'no_machine_specific_asset_path_in_shared_settings': json.loads((folder / 'config/settings.json').read_text())['assetPath'] == '',
}
(output / 'report.json').write_text(json.dumps(checks, indent=2))
print(json.dumps(checks, indent=2))
raise SystemExit(0 if all(checks.values()) else 1)
