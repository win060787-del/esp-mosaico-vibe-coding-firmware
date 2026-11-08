"""Install the local-only voice service. Private pairing material stays outside source."""
import argparse,json,os,plistlib,secrets,shutil,socket,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HOME=Path.home();PRIVATE=HOME/'Library/Application Support/MosaicoCodexPet/voice'
def run(args,**kw):return subprocess.run(args,check=True,**kw)
def install():
 parser=argparse.ArgumentParser();parser.add_argument('--qwen-config',type=Path);args=parser.parse_args()
 PRIVATE.mkdir(parents=True,exist_ok=True);PRIVATE.chmod(0o700)
 candidates=[args.qwen_config,HOME/'Library/Application Support/RemoteVoice/qwen-runtime.json',HOME/'Library/Application Support/mi-ao/qwen-runtime.json']
 model_config=next((p for p in candidates if p and p.is_file()),None)
 if model_config is None:raise SystemExit('请先在遥声设置中配置千问模型')
 q=json.loads(model_config.read_text())
 for k in ('modelPath','pythonPath'):
  p=Path(q[k]).expanduser();q[k]=str(p if p.is_absolute() else model_config.parent/p)
 config_path=PRIVATE/'qwen-runtime.json';config_path.write_text(json.dumps(q));config_path.chmod(0o600)
 app=HOME/'Applications/桌宠语音输入.app';contents=app/'Contents';(contents/'MacOS').mkdir(parents=True,exist_ok=True)
 helper=contents/'MacOS/pet-voice-input'
 run(['xcrun','swiftc','-swift-version','5',*map(str,(ROOT/'companion/voice/swift').glob('*.swift')),'-o',str(helper),'-framework','AppKit','-framework','ApplicationServices'])
 (contents/'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleExecutable='pet-voice-input',CFBundleIdentifier='xyz.novatechai.pet-voice-input',CFBundleName='桌宠语音输入',CFBundleVersion='1',CFBundleShortVersionString='1.0',LSUIElement=True)))
 run(['codesign','--force','--sign','-',str(app)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sock.connect(('192.168.0.1',80));ip=sock.getsockname()[0];sock.close()
 cert=PRIVATE/'server.crt';key=PRIVATE/'server.key';cfgpath=PRIVATE/'service.json'
 old=json.loads(cfgpath.read_text()) if cfgpath.exists() else {}
 if old.get('ip')!=ip or not cert.exists():
  run(['openssl','req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1','-nodes','-keyout',str(key),'-out',str(cert),'-days','3650','-subj','/CN=Codex Pet Voice','-addext',f'subjectAltName=IP:{ip},IP:127.0.0.1'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  key.chmod(0o600)
 name=subprocess.check_output(['scutil','--get','ComputerName'],text=True).strip()
 cfg=dict(ip=ip,port=9876,name=name,token=old.get('token') or secrets.token_hex(32),cert=str(cert),key=str(key),python=q['pythonPath'],qwen_config=str(config_path),input_helper=str(helper))
 cfgpath.write_text(json.dumps(cfg));cfgpath.chmod(0o600)
 agent=HOME/'Library/LaunchAgents/xyz.novatechai.pet-voice.plist';label='xyz.novatechai.pet-voice'
 agent.write_bytes(plistlib.dumps(dict(Label=label,ProgramArguments=[str(ROOT/'.venv/bin/python'),str(ROOT/'companion/voice/service.py'),str(cfgpath)],RunAtLoad=True,KeepAlive=True,StandardOutPath=str(PRIVATE/'service.log'),StandardErrorPath=str(PRIVATE/'service.log'),EnvironmentVariables={'NO_PROXY':'*','no_proxy':'*'})))
 domain=f'gui/{os.getuid()}';subprocess.run(['launchctl','bootout',domain+'/'+label],capture_output=True)
 run(['launchctl','bootstrap',domain,str(agent)],capture_output=True)
 print('Voice service installed; pairing configuration remains private.')
 print(subprocess.check_output([str(helper),'--permission'],text=True).strip())
if __name__=='__main__':install()
