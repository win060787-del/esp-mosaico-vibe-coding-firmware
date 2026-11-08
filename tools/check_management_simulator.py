"""Exercise the shared native C UI through real pointer input and data changes."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from PIL import Image, ImageChops

ROOT=Path(__file__).resolve().parents[1]
PROJECT=ROOT/'vendor/esp-mosaico-vibe-legacy-v26.0.1/projects/codex_pet'
COMPONENT=PROJECT/'managed_components/espressif__esp-gsp'
sys.path.insert(0,str(COMPONENT/'tools/sim_bridge'))
from rpc_client import connect
OUT=ROOT/'design/ui-3.0-simulator'
OUT.mkdir(parents=True,exist_ok=True)

with tempfile.TemporaryDirectory(prefix='codex-pet-ui-') as temporary:
    temp=Path(temporary);ready=temp/'ready.json';state=temp/'state.txt'
    state.write_text('2')
    preferences=temp/'preferences.txt';network=temp/'network.txt';join=temp/'join.txt'
    battery=temp/'battery.txt';battery.write_text('82')
    log=(OUT/'native.log').open('w')
    sim=subprocess.Popen([str(Path.home()/'.cache/esp-mosaico/sim/1.5.1/sim'),
        '--bundle',str(PROJECT/'build-sim/pet_backend_assets/app.gspb'),
        '--api-json',str(PROJECT/'build-sim/pet_backend_assets/bundle_gsp.api.json'),
        '--headless','--frames','0','--backend-enable','--backend-required',
        '--api-listen','tcp://127.0.0.1:0','--ready-file',str(ready)],stdout=log,stderr=log)
    backend=None
    try:
        deadline=time.monotonic()+15
        while not ready.exists():
            if sim.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Simulator failed to start')
            time.sleep(.05)
        endpoints=json.loads(ready.read_text())
        backend=subprocess.Popen([str(PROJECT/'build-sim/pet_backend'),'--endpoint',endpoints['backend'],'--duration-ms','85000'],
            env={**os.environ,'PET_SIM_STATE_FILE':str(state),'PET_SIM_BATTERY_FILE':str(battery),'PET_SIM_PREFS_FILE':str(preferences),'PET_SIM_NET_FILE':str(network),'PET_SIM_JOIN_FILE':str(join)},stdout=log,stderr=log)
        api_port=int(endpoints['api'].rsplit(':',1)[1])
        with connect(port=api_port) as api:
            def call(method,params=None):
                result=api.call(method,params)
                if 'error' in result:raise RuntimeError(result)
                return result.get('result')
            def capture(name):
                time.sleep(1.5)
                call('screenshot',{'path':str(OUT/(name+'.png')),'format':'png'})
                return Image.open(OUT/(name+'.png')).convert('RGB')
            time.sleep(1)
            call('tap_object',{'name':'gear'});capture('hub')
            call('tap_object',{'name':'open_preferences'});f=capture('preferences')
            assert f.getpixel((35,100))[0]>5, 'Preferences page did not open'
            call('tap_object',{'name':'color1'});capture('blue-choice')
            call('tap_object',{'name':'settings_back'});capture('hub-again')
            call('tap_object',{'name':'hub_back'});frame=capture('home-again')
            assert frame.getpixel((240,100))[2]>180,'Hub back did not return to blue pet'
            call('tap_object',{'name':'gear'});capture('hub')
            call('tap_object',{'name':'open_terminals'});capture('terminals')
            call('tap_object',{'name':'mode2'});capture('aggregate')
            assert network.read_text().split()[2]=='2','Aggregate mode not delivered'
            call('tap_object',{'name':'terminal1'});capture('fixed')
            assert network.read_text().split()[2]=='1','Fixed terminal not delivered'
            call('tap_object',{'name':'new_terminal'});capture('enrollment')
            call('tap_object',{'name':'enrollment_back'});capture('terminals-return')
            call('tap_object',{'name':'terminals_back'});capture('hub-return')
            call('tap_object',{'name':'open_wifi'});capture('wifi')
            call('tap_object',{'name':'wifi1'});capture('wifi-detail')
            call('tap_object',{'name':'wifi_delete'});capture('wifi-confirm')
            call('tap_object',{'name':'delete_cancel_button'});capture('wifi-cancel')
            call('tap_object',{'name':'wifi_detail_back'});capture('wifi-return')
            def tap(name):
                call('tap_object',{'name':name})
                time.sleep(.28)
            tap('new_wifi');capture('wifi-scan')
            tap('scan_row0');capture('wifi-entry')
            tap('entry_ok');capture('wifi-join-failure')
            assert join.read_text()=='join failure'
            tap('entry_key0_10')
            tap('entry_upper')
            tap('entry_key1_10')
            tap('entry_symbols')
            tap('entry_key2_10')
            for key in range(1,6):
                tap(f'entry_key2_{key:02}')
            capture('wifi-password-masked')
            tap('entry_ok');capture('wifi-join-success')
            assert join.read_text()=='join success' 
            backend.wait(timeout=65)
            assert backend.returncode==0, "Native management backend failed"
            call('quit')
        print('Management navigation and actions passed')
    finally:
        for p in (backend,sim):
            if p and p.poll() is None:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait()
        log.close()
