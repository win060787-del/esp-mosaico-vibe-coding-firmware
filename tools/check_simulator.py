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
    preferences=temp/'preferences.txt'
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
        backend=subprocess.Popen([str(PROJECT/'build-sim/pet_backend'),'--endpoint',endpoints['backend'],'--duration-ms','90000'],
            env={**os.environ,'PET_SIM_STATE_FILE':str(state),'PET_SIM_BATTERY_FILE':str(battery),'PET_SIM_PREFS_FILE':str(preferences)},stdout=log,stderr=log)
        api_port=int(endpoints['api'].rsplit(':',1)[1])
        with connect(port=api_port) as api:
            def call(method,params=None):
                result=api.call(method,params)
                if 'error' in result:raise RuntimeError(result)
                return result.get('result')
            def capture(name):
                time.sleep(.6)
                call('screenshot',{'path':str(OUT/(name+'.png')),'format':'png'})
                return Image.open(OUT/(name+'.png')).convert('RGB')
            time.sleep(.7)
            deadline=time.monotonic()+12
            while True:
                working=capture('working')
                if working.getpixel((33,442))[2]>180 and any(working.getpixel((x,y))[0]>150 for x in range(134,380) for y in range(432,452)):break
                if time.monotonic()>deadline:raise AssertionError('Completion blue dot not rendered')
            assert any(working.getpixel((x,y))[0]>150 for x in range(134,380) for y in range(432,452)), 'Completion task title missing'
            assert any(working.getpixel((x,y)) != (0,0,0) for x in range(80,120) for y in range(282,307)), 'Brand missing on first live frame'
            eye_samples=[];closed_times=[];last_open=True
            blink_deadline=time.monotonic()+12
            while time.monotonic()<blink_deadline:
                call('screenshot',{'path':str(OUT/'blink.png'),'format':'png'})
                is_open=Image.open(OUT/'blink.png').convert('RGB').getpixel((202,145))[0]>200
                eye_samples.append(is_open)
                if last_open and not is_open:closed_times.append(time.monotonic())
                last_open=is_open
                if len(closed_times)>=2:break
                time.sleep(.06)
            assert any(eye_samples) and not all(eye_samples), 'Blink animation did not close/reopen eyes'
            assert len(closed_times)>=2 and 2.5<closed_times[-1]-closed_times[-2]<4.1,'Blink cadence did not increase by 50 percent'
            call('tap',{'x':240,'y':355})
            detail=capture('tasks')
            assert detail.getpixel((33,442))!=working.getpixel((33,442)),'Tap failed to open task drawer'
            assert detail.getpixel((46,185))[1]>150,'Unread completion green dot missing'
            frames=[]
            for i in range(8):
                frame=capture('spinner-'+str(i));frames.append(frame)
            assert any(ImageChops.difference(frames[0].crop((36,110,56,130)),f.crop((36,110,56,130))).getbbox() for f in frames[1:]),'Spinner did not rotate'
            frames[0].save(OUT/'tasks-animation.gif',save_all=True,append_images=frames[1:],duration=600,loop=0)
            state.write_text('6')
            read_detail=capture('read')
            deadline=time.monotonic()+5
            while read_detail.getpixel((46,185))[1]>150 and time.monotonic()<deadline:read_detail=capture('read')
            assert read_detail.getpixel((46,185))[1]<150,'Read task retained unread green dot'
            state.write_text('2')
            capture('tasks-restored')
            assert any(working.getpixel((x,y))[0]>180 for x in range(180,300) for y in range(24,48)),'Server clock missing'
            call('tap',{'x':50,'y':38})
            returned=capture('back')
            assert returned.getpixel((33,442))==working.getpixel((33,442)),'Back failed'
            call('tap',{'x':44,'y':35});capture('settings-hub')
            call('tap_object',{'name':'open_preferences'});capture('settings')
            call('tap',{'x':180,'y':155});time.sleep(.5)
            call('tap',{'x':380,'y':155});time.sleep(.5)
            call('tap',{'x':370,'y':225});time.sleep(.5)
            call('tap',{'x':210,'y':330});time.sleep(.5)
            values=[int(x) for x in preferences.read_text().split()]
            assert 20<values[0]<80 and 20<values[1]<80 and values[2]==0 and values[3]>=1,'Settings controls failed'
            call('drag',{'x1':180,'y1':155,'x2':117,'y2':155});time.sleep(.5)
            call('tap',{'x':380,'y':102});time.sleep(.5)
            values=[int(x) for x in preferences.read_text().split()]
            assert 20<values[0]<40 and values[4]==0,'Volume drag or sound switch did not reach controls'
            previews=values[3]
            call('tap',{'x':380,'y':155});time.sleep(.3)
            assert int(preferences.read_text().split()[3])==previews,'Sound-off preview remained enabled'
            capture('settings-adjusted')
            call('tap',{'x':50,'y':38});capture('settings-back')
            call('tap_object',{'name':'hub_back'});time.sleep(.3)
            palette=[(112,40,232),(22,119,255),(23,184,151),(217,119,6),(217,79,157),(22,157,185)]
            for i,rgb in enumerate(palette):
                call('tap',{'x':44,'y':35});time.sleep(1)
                call('tap_object',{'name':'open_preferences'})
                deadline=time.monotonic()+3
                while True:
                    panel=capture('palette-'+str(i))
                    if panel.getpixel((35,100))[0]>5:break
                    if time.monotonic()>deadline:raise AssertionError('Settings did not open for color choice')
                call('tap_object',{'name':'color'+str(i)})
                deadline=time.monotonic()+3
                while True:
                    values=[int(x) for x in preferences.read_text().split()]
                    if len(values)==6 and values[5]==i:break
                    if time.monotonic()>deadline:raise AssertionError(('Color input not delivered',i,values))
                    time.sleep(.1)
                call('tap_object',{'name':'settings_back'});time.sleep(1)
                call('tap_object',{'name':'hub_back'})
                deadline=time.monotonic()+3
                while True:
                    colored=capture('color-'+str(i))
                    actual=colored.getpixel((240,100))
                    if all(abs(actual[j]-rgb[j])<10 for j in range(3)):break
                    if time.monotonic()>deadline:raise AssertionError(('Pet color did not apply',i,actual))
            for value,name in [(3,'waiting'),(4,'completed'),(5,'failed'),(0,'offline')]:
                state.write_text(str(value))
                image=capture(name)
                if name=='offline':assert image.getpixel((33,442))[2]<30,'Offline retained current completion marker'
            for percentage,name in [(82,'green'),(25,'yellow'),(8,'red'),(-1,'unknown')]:
                battery.write_text(str(percentage));time.sleep(1.3)
                frame=capture('battery-'+name)
                r,g,b=frame.getpixel((365,36))
                if name=='green':assert g>120 and g>r and g>b,'Battery green missing'
                elif name=='yellow':assert r>180 and g>120 and b<80,'Battery yellow missing'
                elif name=='red':assert r>180 and g<100,'Battery red missing'
                else:assert max(r,g,b)<30,'Unknown battery retained a false level'
            backend.wait(timeout=95)
            assert backend.returncode == 0, 'Native backend failed'
            call('quit')
        print('Native UI: spinner animation, unread green dot clearing, 50 percent faster blink, completion title, clock, face tap, back and lifecycle states passed')
    finally:
        for p in (backend,sim):
            if p and p.poll() is None:
                p.terminate()
                try:p.wait(timeout=5)
                except subprocess.TimeoutExpired:p.kill();p.wait()
        log.close()
