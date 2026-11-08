"""Install the local USB companion as a user login service."""
from pathlib import Path
import os
import plistlib
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
LABEL = 'com.novatechai.mosaico-codex-pet'
DEVICE = '4553502d4952495301001c2904d0a33a'

def main():
    logs = Path.home() / 'Library/Logs/MosaicoCodexPet'
    logs.mkdir(parents=True, exist_ok=True)
    agents = Path.home() / 'Library/LaunchAgents'
    agents.mkdir(parents=True, exist_ok=True)
    plist = agents / (LABEL + '.plist')
    config = {
        'Label': LABEL,
        'ProgramArguments': [str(ROOT/'companion/run.sh')],
        'WorkingDirectory': str(ROOT),
        'RunAtLoad': True,
        'KeepAlive': {'SuccessfulExit': False},
        'ThrottleInterval': 15,
        'StandardOutPath': str(logs/'companion.log'),
        'StandardErrorPath': str(logs/'error.log'),
    }
    domain = 'gui/' + str(os.getuid())
    subprocess.run(['launchctl', 'bootout', domain+'/'+LABEL], capture_output=True)
    deadline=time.monotonic()+5
    while subprocess.run(['launchctl','print',domain+'/'+LABEL],capture_output=True).returncode==0:
        if time.monotonic()>deadline:raise RuntimeError('Previous companion service is still stopping')
        time.sleep(0.1)
    plist.write_bytes(plistlib.dumps(config))
    plist.chmod(0o600)
    subprocess.run(['launchctl', 'bootstrap', domain, str(plist)], check=True)
    print('Installed:', plist)

if __name__ == '__main__':
    main()
