"""Provision device settings through the official CLI; never log credentials."""
from pathlib import Path
import base64,json,subprocess
ROOT=Path(__file__).resolve().parents[1]
WORKSPACE=ROOT/'vendor/esp-mosaico-vibe-legacy-v26.0.1'
DEVICE='4553502d4952495301001c2904d0a33a'
PRIVATE=Path.home()/'Library/Application Support/MosaicoCodexPet/cloud.json'

def rpc(method,payload=None):
 args=[str(ROOT/'.venv/bin/python'),str(WORKSPACE/'mosaico.py'),'iris','rpc','0x70',str(method),'--project','projects/codex_pet','--device-id',DEVICE]
 if payload is not None:args+=['--payload-hex',json.dumps(payload,ensure_ascii=False).encode().hex()]
 # Hex payloads go to Gateway over stdin; its operation journal retains only a hash.
 args+=['--json']
 result=subprocess.run(args,cwd=WORKSPACE,capture_output=True,text=True,timeout=25)
 if result.returncode:raise RuntimeError('Device configuration failed; check USB connection')
 data=json.loads(result.stdout)
 body=data.get('response',{}).get('payload_base64')
 return base64.b64decode(body).decode() if body else data

def configure(ssid=None,password=None,use_recovery_wifi=False):
 cfg=json.loads(PRIVATE.read_text());payload={'url':cfg['url'],'token':cfg['read']}
 if ssid is not None:payload.update(ssid=ssid,password=password or '')
 if use_recovery_wifi:payload['use_recovery_wifi']=True
 return rpc(2,payload)

if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser();parser.add_argument('--configure',action='store_true');parser.add_argument('--use-saved-wifi',action='store_true');args=parser.parse_args()
 print(configure(use_recovery_wifi=args.use_saved_wifi) if args.configure else rpc(3))
