"""Paired LAN updates through the official retained-Recovery product workflow."""
import argparse,base64,ipaddress,json,secrets,socket,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
WORKSPACE=ROOT/'vendor/esp-mosaico-vibe-legacy-v26.0.1'
PROJECT=WORKSPACE/'projects/codex_pet'
PRIVATE=Path.home()/'Library/Application Support/MosaicoCodexPet'
DEVICE='4553502d4952495301001c2904d0a33a'
HOSTNAME='codex-pet-d0a33a.local'

def provision():
    from device_network import rpc
    PRIVATE.mkdir(parents=True,exist_ok=True);PRIVATE.chmod(0o700)
    token_path=PRIVATE/'wireless.token'
    token=token_path.read_text().strip() if token_path.exists() else secrets.token_hex(32)
    if len(token)!=64 or any(c not in '0123456789abcdef' for c in token):raise RuntimeError('Invalid private pairing file')
    token_path.write_text(token+'\n');token_path.chmod(0o600)
    # Binary input is forwarded by the product CLI over stdin and logged as a hash.
    result=subprocess.run([str(ROOT/'.venv/bin/python'),str(WORKSPACE/'mosaico.py'),'iris','rpc','0x70','7',
        '--project','projects/codex_pet','--device-id',DEVICE,'--payload-hex',token.encode().hex(),'--json'],
        cwd=WORKSPACE,capture_output=True,text=True,timeout=25)
    if result.returncode:raise RuntimeError('USB pairing failed; device must be connected locally')
    info=json.loads(rpc(3));address=ipaddress.ip_address(info['ip'])
    if address.is_unspecified or not (address.is_private or address.is_link_local):raise RuntimeError('No local Wi-Fi address')
    config={'device_id':DEVICE,'hostname':HOSTNAME,'endpoint':'tcp:'+str(address)+':19772','token_file':str(token_path)}
    path=PRIVATE/'wireless.json';path.write_text(json.dumps(config,indent=2)+'\n');path.chmod(0o600)
    print('Wireless pairing saved privately; device Wi-Fi is ready.')

def load_config():
    config=json.loads((PRIVATE/'wireless.json').read_text())
    if config.get('device_id')!=DEVICE:raise RuntimeError('Unexpected paired device')
    try:
        address=ipaddress.ip_address(socket.gethostbyname(config['hostname']))
        if not (address.is_private or address.is_link_local):raise RuntimeError('Not a LAN endpoint')
        config['endpoint']='tcp:'+str(address)+':19772'
    except socket.gaierror:pass
    return config

def update(bundle):
    config=load_config();bundle=Path(bundle).expanduser().resolve()
    if bundle.suffix!='.irisfw' or not bundle.is_file():raise RuntimeError('Choose a valid .irisfw bundle')
    args=[str(ROOT/'.venv/bin/python'),str(WORKSPACE/'mosaico.py'),'iris','system-update',
        '--device-id',DEVICE,'--endpoint',config['endpoint'],'--pairing-token-file',config['token_file'],
        '--bundle',str(bundle),'--timeout','300']
    return subprocess.run(args,cwd=PROJECT).returncode

def main():
    parser=argparse.ArgumentParser();parser.add_argument('bundle',nargs='?');parser.add_argument('--pair',action='store_true');args=parser.parse_args()
    if args.pair:provision();return
    bundle=args.bundle
    if not bundle:
        chosen=subprocess.run(['osascript','-e','POSIX path of (choose file with prompt "选择桌宠固件（.irisfw）")'],capture_output=True,text=True)
        if chosen.returncode:return
        bundle=chosen.stdout.strip()
    raise SystemExit(update(bundle))
if __name__=='__main__':main()
