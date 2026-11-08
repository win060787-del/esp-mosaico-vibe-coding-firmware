from pathlib import Path
import shutil,zipfile,sys
r=Path.cwd();out=r/'dist/Mac桌宠采集端';out.mkdir(exist_ok=True)
for sub in ['companion','server']:(out/sub).mkdir(exist_ok=True)
shutil.copytree(r/'companion/mosaico_pet',out/'companion/mosaico_pet',dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
shutil.copyfile(r/'server/server.crt',out/'server/server.crt')
shutil.copyfile(r/'packaging/install_reporter.py',out/'install_reporter.py')
base=Path(sys.base_prefix)
if sys.version_info[:2]!=(3,12):raise RuntimeError('Package with the verified Python 3.12 runtime')
(out/'runtime/bin').mkdir(parents=True,exist_ok=True)
shutil.copy2(base/'bin/python3.12',out/'runtime/bin/python3.12')
shutil.copytree(base/'lib/python3.12',out/'runtime/lib/python3.12',dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','site-packages','test','tests','*.pyc'))
command=out/'安装采集端.command';command.write_text('''#!/bin/sh
set -eu
cd "$(dirname "$0")"
if [ "$(uname -m)" = arm64 ]; then
 exec ./runtime/bin/python3.12 ./install_reporter.py
fi
for python in /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
 if [ -x "$python" ] && "$python" -c 'import sys; assert sys.version_info >= (3,9)' 2>/dev/null; then
  exec "$python" ./install_reporter.py
 fi
done
osascript -e 'display dialog "这台 Intel Mac 需要先安装 Python 3.9 或更新版本，再打开采集端安装程序。" buttons {"好"}'
''');command.chmod(0o755)
(out/'使用说明.txt').write_text('安装前请先安装并登录 Codex。\n双击「安装采集端.command」，输入终端名称和桌宠设置页生成的配对码。\nApple 芯片 Mac 已自带运行环境；Intel Mac 需要已有 Python 3.9 或更新版本。\n安装后自动登录运行，电脑需要开机联网。密码、服务器管理密钥均不包含在此安装包中。\n')
shutil.make_archive(str(r/'dist/Mac桌宠采集端'),'zip',r/'dist',out.name)
print('Mac reporter package created without credentials.')
