"""Capture documentation screens from the same native UI used by firmware."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = Path(os.environ.get('MOSAICO_WORKSPACE', ROOT.parent))
PROJECT = WORKSPACE / 'vendor/esp-mosaico-vibe-legacy-v26.0.1/projects/codex_pet'
sys.path.insert(0, str(PROJECT / 'managed_components/espressif__esp-gsp/tools/sim_bridge'))
from rpc_client import connect

OUT = ROOT / 'docs/screenshots'
OUT.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix='pet-docs-') as work:
    tmp = Path(work)
    ready = tmp / 'ready.json'
    state = tmp / 'state.txt'
    state.write_text('2')
    battery = tmp / 'battery.txt'
    battery.write_text('82')
    sim = subprocess.Popen([
        str(Path.home() / '.cache/esp-mosaico/sim/1.5.1/sim'),
        '--bundle', str(PROJECT / 'build-sim/pet_backend_assets/app.gspb'),
        '--api-json', str(PROJECT / 'build-sim/pet_backend_assets/bundle_gsp.api.json'),
        '--headless', '--frames', '0', '--backend-enable', '--backend-required',
        '--api-listen', 'tcp://127.0.0.1:0', '--ready-file', str(ready),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    backend = None
    try:
        deadline = time.monotonic() + 15
        while not ready.exists():
            if sim.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Simulator did not start')
            time.sleep(.05)
        endpoints = json.loads(ready.read_text())
        backend = subprocess.Popen([
            str(PROJECT / 'build-sim/pet_backend'), '--endpoint', endpoints['backend'],
            '--duration-ms', '65000',
        ], env={**os.environ, 'PET_SIM_STATE_FILE': str(state),
                'PET_SIM_BATTERY_FILE': str(battery)},
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port = int(endpoints['api'].rsplit(':', 1)[1])
        with connect(port=port) as api:
            def call(method, params=None):
                result = api.call(method, params)
                if 'error' in result:
                    raise RuntimeError(result)
                return result.get('result')

            def capture(name):
                time.sleep(.7)
                call('screenshot', {'path': str(OUT / (name + '.png')), 'format': 'png'})
                print(name, flush=True)

            def tap(name):
                call('tap_object', {'name': name})
                time.sleep(1)

            def xy(x, y):
                call('tap', {'x': x, 'y': y})
                time.sleep(1)

            time.sleep(2)
            capture('home-loading')
            xy(200, 365)
            capture('tasks')
            tap('back')
            state.write_text('7')
            time.sleep(1)
            capture('recent')
            state.write_text('2')
            time.sleep(1)
            capture('home')
            tap('face')
            capture('voice')
            tap('voice_back')
            tap('gear')
            capture('settings-hub')
            tap('language')
            capture('settings-english')
            tap('hub_back')
            capture('home-english')
            tap('face')
            capture('voice-english')
            tap('voice_back')
            tap('gear')
            tap('language')
            xy(210, 120)
            capture('settings')
            xy(45, 39)
            xy(210, 205)
            capture('terminals')
            xy(105, 445)
            capture('terminal-pairing')
            xy(45, 39)
            xy(45, 39)
            xy(210, 285)
            capture('wifi')
            xy(90, 445)
            capture('wifi-scan')
            xy(210, 100)
            capture('wifi-password')
            call('quit')
    finally:
        for process in (backend, sim):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
