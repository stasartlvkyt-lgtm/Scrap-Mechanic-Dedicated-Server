import csv
import json
import pathlib
import subprocess
import sys
import tempfile

exe = pathlib.Path(sys.argv[1]).resolve()
def run(*args, ok=True):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    if (result.returncode == 0) != ok:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result

status = json.loads(run('status').stdout)
assert status['vanilla_client_join'] and status['vanilla_client_join_observed']
assert status['native_joint_paint_explicit_color'] and status['joint_paint_default_color']
assert not status['vanilla_gameplay_command_verified'] and not status['dedicated_server_complete']
run('serve', ok=False)
assert '--seconds N|0' in run('--help').stdout
assert '--native-players' in run('--help').stdout
assert '--native-physics' in run('--help').stdout
run('simulate-world', 'nonexistent.db', '--ticks', '0', ok=False)
run('simulate-world', 'nonexistent.db', '--ticks', '-1', ok=False)
run('inspect', 'nonexistent.db', ok=False)
run('simulate', '--ticks', '-1', ok=False)
run('simulate', '--ticks', '2x', ok=False)
run('simulate', '--typo', '4', ok=False)
with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    trace = root / 'trace.csv'
    result = json.loads(run('simulate', '--ticks', '400', '--trace', trace).stdout)
    assert abs(result['bodies'][0]['position'][2] - 0.5) < 0.02
    with trace.open() as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 401 and rows[0]['tick'] == '0' and rows[-1]['tick'] == '400'
    before = trace.read_bytes()
    run('simulate', '--trace', trace, ok=False)
    assert trace.read_bytes() == before
    scenario = root / 'сценарий.lua'
    scenario.write_text('return {name="test", gravity={z=0}, bodies={{id=7,position={z=3}}}}')
    result = json.loads(run('simulate', '--scenario', scenario, '--ticks', '100').stdout)
    assert result['bodies'][0]['id'] == 7 and result['bodies'][0]['position'][2] == 3
    scenario.write_text('#!/usr/bin/env lua\nreturn {bodies={{id=7}}}')
    assert json.loads(run('simulate', '--scenario', scenario, '--ticks', '0').stdout)['bodies'][0]['id'] == 7
    scenario.write_text('return {bodies={{id=1},{id=1}}}')
    run('simulate', '--scenario', scenario, ok=False)
    scenario.write_text('while true do end')
    run('simulate', '--scenario', scenario, ok=False)
    scenario.write_bytes(b' ' * (4 * 1024 * 1024 + 1))
    assert 'exceeds 4 MiB' in run('simulate', '--scenario', scenario, ok=False).stderr
print('CLI checks passed')
