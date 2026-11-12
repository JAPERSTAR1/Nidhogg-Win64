"""Read only this RG's costs; deallocate only this VM at a threshold/deadline.

Uses a system-assigned identity with a two-action custom role. No operator
credentials are stored on the VM. Budgets notify; this timer performs the stop.
"""
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys
import urllib.parse
import urllib.request

SUB = os.environ['AZURE_SUBSCRIPTION_ID']
GROUP = os.environ['AZURE_RESOURCE_GROUP']
VM = os.environ['AZURE_VM_NAME']
DEADLINE = datetime.fromisoformat(os.environ['NIDHOGG_STOP_AT_UTC'])
LIMIT = float(os.environ.get('NIDHOGG_REPORTED_COST_LIMIT_USD', '65'))
SCOPE = f'/subscriptions/{SUB}/resourceGroups/{GROUP}'
dry_run = '--dry-run' in sys.argv
local_only = '--local-only' in sys.argv
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
def arm_headers():
    metadata = 'http://169.254.169.254/metadata/identity/oauth2/token?' + urllib.parse.urlencode({
        'api-version': '2018-02-01', 'resource': 'https://management.azure.com/'})
    with opener.open(urllib.request.Request(metadata, headers={'Metadata': 'true'}), timeout=10) as response:
        token = json.load(response)['access_token']
    return {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'}

now = datetime.now(timezone.utc)
start = datetime.fromisoformat(os.environ['NIDHOGG_BUDGET_START_UTC'])
estimated = max(0, (now - start).total_seconds() / 3600) * float(os.environ['NIDHOGG_VM_HOURLY_USD'])
estimated += float(os.environ.get('NIDHOGG_NONCOMPUTE_RESERVE_USD', '20'))
estimated_limit = float(os.environ.get('NIDHOGG_ESTIMATED_TOTAL_LIMIT_USD', '85'))
# Count the default interface only, including TLS/SSH overhead, across reboots.
state_dir = Path('/var/lib/nidhogg-guard'); state_dir.mkdir(exist_ok=True)
state_file = state_dir / 'egress.json'
counter = int((Path('/sys/class/net') / os.environ.get('NIDHOGG_NETWORK_INTERFACE', 'eth0') / 'statistics/tx_bytes').read_text())
state = json.loads(state_file.read_text()) if state_file.exists() else {'last': 0, 'total': 0}
state['total'] += counter - state['last'] if counter >= state['last'] else counter
state['last'] = counter
temp = state_file.with_suffix('.tmp'); temp.write_text(json.dumps(state)); temp.replace(state_file)
egress_limit = float(os.environ.get('NIDHOGG_EGRESS_LIMIT_GIB', '80')) * 1024**3
stop = now >= DEADLINE or estimated >= estimated_limit or state['total'] >= egress_limit
cost = None
try:
    if local_only: raise StopIteration
    headers = arm_headers()
    body = {'type': 'ActualCost', 'timeframe': 'MonthToDate', 'dataset': {
        'granularity': 'None', 'aggregation': {'total': {'name': 'PreTaxCost', 'function': 'Sum'}}}}
    request = urllib.request.Request('https://management.azure.com' + SCOPE +
        '/providers/Microsoft.CostManagement/query?api-version=2025-03-01',
        data=json.dumps(body).encode(), headers=headers, method='POST')
    with opener.open(request, timeout=30) as response:
        result = json.load(response)['properties']
    columns = [c['name'] for c in result['columns']]
    rows = result.get('rows', [])
    currency_index = columns.index('Currency') if 'Currency' in columns else None
    if currency_index is not None:
        assert all(row[currency_index] == 'USD' for row in rows), 'Unexpected cost currency'
    cost_index = next(i for i,c in enumerate(columns) if c in ['PreTaxCost', 'Cost', 'total'])
    cost = sum(float(row[cost_index]) for row in rows)
    stop = stop or cost >= LIMIT
except StopIteration:
    pass
except Exception as error:
    # Cost data can lag; the independent date deadline remains enforced.
    print('Reported cost unavailable:', type(error).__name__, 'status=', getattr(error, 'code', None))
print(json.dumps({'reported_month_to_date_usd': cost, 'reported_cost_limit_usd': LIMIT,
                  'estimated_total_with_reserve_usd': round(estimated, 4), 'estimated_total_limit_usd': estimated_limit,
                  'counted_egress_gib': round(state['total'] / 1024**3, 5),
                  'deadline_utc': DEADLINE.isoformat(), 'stop_required': stop, 'dry_run': dry_run}))
if stop and not dry_run:
    headers = arm_headers()
    endpoint = 'https://management.azure.com' + SCOPE + f'/providers/Microsoft.Compute/virtualMachines/{VM}/deallocate?api-version=2024-11-01'
    with opener.open(urllib.request.Request(endpoint, data=b'', headers=headers, method='POST'), timeout=30) as response:
        assert response.status in [200, 202, 204]
    print('Azure VM deallocation requested; disk and public IP charges can continue.')
