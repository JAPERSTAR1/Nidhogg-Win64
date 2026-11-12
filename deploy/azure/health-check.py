"""Local recovery without an external monitoring subscription."""
import json
from pathlib import Path
import subprocess
import urllib.request

state = Path('/var/lib/nidhogg/health-failures')
try:
    with urllib.request.urlopen('http://127.0.0.1:27880/health', timeout=4) as response:
        assert response.status == 200 and json.load(response)['ok']
    state.write_text('0')
except Exception as error:
    failures = int(state.read_text()) + 1 if state.exists() else 1
    state.write_text(str(failures))
    print('Lobby health check failed:', type(error).__name__, 'consecutive=', failures)
    if failures >= 3:
        subprocess.run(['systemctl', 'restart', 'nidhogg'], check=True)
        state.write_text('0')
