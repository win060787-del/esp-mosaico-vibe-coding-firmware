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
OUT=ROOT/'design/ui-3.2-voice'
OUT.mkdir(parents=True,exist_ok=True)

with tempfile.TemporaryDirectory(prefix='codex-pet-ui-') as temporary:
    temp=Path(temporary);ready=temp/'ready.json';state=temp/'state.txt'
    state.write_text('2')
    preferences=temp/'preferences.txt';network=temp/'network.txt';join=temp/'join.txt';voice=temp/'voice.txt'
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
        backend=subprocess.Popen([str(PROJECT/'build-sim/pet_backend'),'--endpoint',endpoints['backend'],'--duration-ms','35000'],
            env={**os.environ,'PET_SIM_STATE_FILE':str(state),'PET_SIM_BATTERY_FILE':str(battery),'PET_SIM_PREFS_FILE':str(preferences),'PET_SIM_NET_FILE':str(network),'PET_SIM_JOIN_FILE':str(join),'PET_SIM_VOICE_FILE':str(voice)},stdout=log,stderr=log)
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
            call('tap_object',{'name':'face'});capture('voice-ready')
            call('tap_object',{'name':'voice_record'});capture('voice-recording')
            call('tap_object',{'name':'voice_delete'});capture('voice-delete')
            assert 'delete' in voice.read_text().splitlines()
            call('tap_object',{'name':'voice_record'});capture('voice-resumed')
            call('tap_object',{'name':'voice_send'});capture('voice-sent')
            assert voice.read_text().splitlines()[-1]=='send'
            call('tap_object',{'name':'voice_back'});home=capture('home-return')
            for point in [(240,355),(240,440)]:
                call('tap',{'x':point[0],'y':point[1]});drawer=capture('tasks-'+str(point[1]))
                assert ImageChops.difference(home,drawer).getbbox(), 'Information region did not open tasks'
                call('tap_object',{'name':'back'});capture('home-'+str(point[1]))
            state.write_text('7');fallback=capture('recent-fallback')
            assert fallback.getpixel((33,442))[2]<180, 'Fallback incorrectly uses completion dot'
            assert any(fallback.getpixel((x,y))[0]>150 for x in range(134,400) for y in range(432,452)), 'Recent interaction missing'
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
