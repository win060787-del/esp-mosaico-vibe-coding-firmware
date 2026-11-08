"""Private paired TCP RPC transport; payloads are never printed."""
import base64,json,subprocess
from wireless_update import ROOT,WORKSPACE,PROJECT,DEVICE,load_config

def rpc(method,payload=None):
    cfg=load_config()
    args=[str(ROOT/'.venv/bin/python'),str(WORKSPACE/'mosaico.py'),'iris','rpc','0x70',str(method),'--device-id',DEVICE,'--endpoint',cfg['endpoint'],'--pairing-token-file',cfg['token_file'],'--project',str(PROJECT),'--json']
    if payload is not None:args+=['--payload-hex',json.dumps(payload,ensure_ascii=False).encode().hex()]
    r=subprocess.run(args,cwd=WORKSPACE,capture_output=True,text=True,timeout=30)
    if r.returncode:raise RuntimeError('Paired device RPC failed')
    data=json.loads(r.stdout);body=data.get('response',{}).get('payload_base64')
    return base64.b64decode(body).decode() if body else data
