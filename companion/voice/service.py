"""Authenticated LAN dictation; audio lives in bounded RAM and temporary 0600 WAVs.
The worker exclusively owns ASR and editor access. HTTP threads only queue commands/audio.
"""
import array, collections, hmac, http.server, json, os, queue, select, socket, ssl
import subprocess, tempfile, threading, time, uuid, wave
from pathlib import Path

RATE=16000
MAX_AUDIO=RATE*2*60

class JSONProcess:
    def __init__(self,args):
        self.process=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0)
    def read(self,timeout=90):
        end=time.monotonic()+timeout; line=bytearray()
        while time.monotonic()<end:
            if not select.select([self.process.stdout],[],[],max(0,end-time.monotonic()))[0]:break
            b=self.process.stdout.read(1)
            if not b:raise RuntimeError('本地语音组件已退出')
            if b==b'\n':
                try:return json.loads(line)
                except ValueError:line.clear();continue
            line+=b
            if len(line)>16384:raise RuntimeError('语音组件返回无效')
        self.process.kill();raise RuntimeError('本地语音处理超时')
    def request(self,value):
        value=dict(value,id=uuid.uuid4().hex)
        self.process.stdin.write(json.dumps(value,ensure_ascii=False).encode()+b'\n');self.process.stdin.flush()
        result=self.read()
        if result.get('id')!=value['id'] or result.get('error'):raise RuntimeError(result.get('error','语音组件响应无效'))
        return result

class InputApplication:
    def __init__(self,config):
        self.socket=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.socket.settimeout(10)
        path=str(Path(config['qwen_config']).with_name('input.sock'))
        try:self.socket.connect(path)
        except OSError:
            self.socket.close()
            subprocess.run(['open','-g','-a',str(Path(config['input_helper']).parents[2])],check=True)
            end=time.monotonic()+15
            while True:
                self.socket=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.socket.settimeout(10)
                try:self.socket.connect(path);break
                except OSError:
                    self.socket.close()
                    if time.monotonic()>end:raise RuntimeError('桌宠语音输入应用未启动')
                    time.sleep(.2)
        self.stream=self.socket.makefile('rwb',buffering=0)
    def request(self,value):
        value=dict(value,id=uuid.uuid4().hex)
        self.stream.write(json.dumps(value,ensure_ascii=False).encode()+b'\n')
        line=self.stream.readline(16384)
        if not line:raise RuntimeError('输入应用已断开')
        result=json.loads(line)
        if result.get('id')!=value['id']:raise RuntimeError('输入应用响应不匹配')
        return result

class Engine:
    def __init__(self,config): self.config=config;self.asr=None;self.editor=None
    def warm(self):
        if self.asr is None:
            self.asr=JSONProcess([self.config['python'],'-u',str(Path(__file__).with_name('qwen_worker.py')),self.config['qwen_config']])
            ready=self.asr.read()
            if not ready.get('ready'):
                self.asr.process.terminate();self.asr=None
                raise RuntimeError('语音模型未就绪：'+str(ready.get('error','未知错误')))
        if self.editor is None:self.editor=InputApplication(self.config)
    def edit(self,action,session,text=''):
        result=self.editor.request(dict(action=action,session=session,text=text))
        if not result.get('ok'):
            error=result.get('error','输入框操作失败')
            if error=='Codex 未运行或遥声未获辅助功能授权':error='Mac 输入权限未生效或 Codex 未打开'
            raise RuntimeError(error)
    def transcribe(self,pcm):
        fd,name=tempfile.mkstemp(suffix='.wav',prefix='pet-voice-')
        try:
            with os.fdopen(fd,'wb') as f:
                with wave.open(f,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(RATE);w.writeframes(pcm)
            return self.asr.request({'audio':name})['text'].strip()
        finally:os.unlink(name)

class Session:
    def __init__(self,engine,name):
        self.engine=engine;self.name=name;self.lock=threading.RLock();self.commands=queue.Queue(maxsize=8)
        self.state='idle';self.text='';self.error='';self.session='';self.prefix='';self.pcm=bytearray();self.seq=0
        self.processed=0;self.last_audio=0;self.last_partial=0;self.revision=0;self.level=0;self.audio_chunks=0;self.last_failure=''
        self.owner='';self.requests=collections.OrderedDict();self.sending=False
    def snapshot(self):
        with self.lock:return dict(state=self.state,text=self.text,error=self.error,name=self.name,revision=self.revision,level=self.level,audio_chunks=self.audio_chunks,last_failure=self.last_failure)
    def command(self,body):
        action=body.get('action');key=body.get('request');device=body.get('device')
        if action not in ('start','stop','delete','clear','send','leave') or not isinstance(key,str) or len(key)>64 or not isinstance(device,str) or not device or len(device)>64:raise ValueError('无效命令')
        with self.lock:
            if self.owner and self.owner!=device:raise ValueError('另一台桌宠正在输入')
            if key in self.requests:return self.snapshot()
            self.commands.put_nowait(action);self.owner=device;self.requests[key]=True
            while len(self.requests)>128:self.requests.popitem(last=False)
            return self.snapshot()
    def audio(self,device,sequence,pcm):
        if not pcm or len(pcm)>19200 or len(pcm)%2:raise ValueError('无效音频')
        with self.lock:
            if device!=self.owner or self.state!='recording':raise ValueError('未开始录音')
            if sequence<self.seq:return self.snapshot() # Duplicate upload does not duplicate samples.
            if sequence!=self.seq:raise ValueError('音频顺序错误，请重新录音')
            if len(self.pcm)+len(pcm)>MAX_AUDIO:raise ValueError('本段录音达到 60 秒')
            self.pcm.extend(pcm);self.seq+=1;self.audio_chunks+=1;self.last_audio=time.monotonic()
            samples=array.array('h',pcm);self.level=min(100,max(abs(x) for x in samples)*100//10000)
            return self.snapshot()
    def change(self,state,error=''):
        with self.lock:
            self.state=state;self.error=error;self.revision+=1
            if error:self.last_failure=error
        print('voice state='+state+' error='+error,flush=True)
    def recognize(self,final=False):
        with self.lock: pcm=bytes(self.pcm);prefix=self.prefix
        if len(pcm)<3200:return
        text=self.engine.transcribe(pcm)
        if not text:return
        combined=prefix+text
        if len(combined.encode())>1800:raise RuntimeError('听写文字已满，请发送后继续')
        self.engine.edit('update',self.session,combined)
        with self.lock:self.text=combined;self.processed=len(pcm);self.revision+=1
    def finish(self):
        if self.state=='recording':
            self.change('processing');self.recognize(True)
            with self.lock:self.pcm.clear();self.level=0
            self.change('ready')
    def step(self):
        try:action=self.commands.get_nowait()
        except queue.Empty:action=None
        if action:
            if action=='start':
                if self.state=='recording':return
                if self.sending:raise RuntimeError('上次发送结果不确定，请先查看 Mac')
                if self.state=='error' and self.session:
                    self.engine.edit('end',self.session)
                    self.session='';self.text='';self.prefix=''
                self.change('warming');self.engine.warm()
                if not self.session:
                    new=str(uuid.uuid4());self.engine.edit('begin',new);self.session=new
                self.prefix=self.text
                with self.lock:self.pcm.clear();self.seq=0;self.processed=0;self.last_audio=time.monotonic();self.last_partial=0
                self.change('recording')
            elif action in ('stop','delete','clear','send','leave'):
                self.finish()
                if action in ('delete','clear') and self.session:
                    # Remove one extended grapheme cluster, including emoji.
                    if action=='clear':next_text=''
                    else:
                        import regex
                        next_text=''.join(regex.findall(r'\X',self.text)[:-1])
                    self.engine.edit('update',self.session,next_text);self.text=next_text;self.change('ready')
                elif action=='send':
                    if not self.text or not self.session:raise RuntimeError('没有可发送的听写内容')
                    if self.sending:raise RuntimeError('上次发送结果不确定，请先查看 Mac')
                    self.sending=True;self.change('sending');self.engine.edit('send',self.session)
                    self.session='';self.text='';self.prefix='';self.owner='';self.sending=False;self.change('sent')
                elif action=='leave':
                    # End editing ownership, preserve existing Mac draft.
                    if self.session:self.engine.edit('end',self.session)
                    self.session='';self.text='';self.owner='';self.sending=False;self.change('idle')
        elif self.state=='recording':
            now=time.monotonic()
            if now-self.last_audio>6:
                self.finish();raise RuntimeError('录音连接中断，已保留文字')
            if len(self.pcm)-self.processed>=19200 and now-self.last_partial>=0.6:
                self.last_partial=now;self.recognize()
    def run(self):
        while True:
            try:self.step()
            except Exception as e:
                with self.lock:self.pcm.clear();self.level=0
                self.change('error',str(e)[:180])
            time.sleep(.025)

def serve(config):
    session=Session(Engine(config),config['name'])
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args):pass
        def setup(self):
            super().setup();self.connection.settimeout(8)
            self.connection.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        def respond(self,status,payload):
            data=json.dumps(payload,ensure_ascii=False).encode();self.send_response(status)
            self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        def do_POST(self):
            if not hmac.compare_digest(self.headers.get('Authorization',''),'Bearer '+config['token']):
                self.close_connection=True;self.respond(401,{'error':'unauthorized'});return
            try:
                size=int(self.headers.get('Content-Length','0'))
                if size<1 or size>19200:raise ValueError('invalid length')
                body=self.rfile.read(size)
                if len(body)!=size:raise ValueError('incomplete upload')
                if self.path=='/audio':response=session.audio(self.headers.get('X-Device',''),int(self.headers.get('X-Sequence','-1')),body)
                elif self.path=='/control':response=session.command(json.loads(body))
                elif self.path=='/state':response=session.snapshot()
                else:raise ValueError('invalid endpoint')
                self.respond(200,response)
            except (ValueError,queue.Full) as e:
                response=session.snapshot()
                if not response['error']:response['error']=str(e) or '操作繁忙，请稍后再试'
                self.respond(409,response)
    server=http.server.ThreadingHTTPServer(('0.0.0.0',config['port']),Handler)
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.minimum_version=ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(config['cert'],config['key']);server.socket=context.wrap_socket(server.socket,server_side=True)
    threading.Thread(target=session.run,daemon=True).start();server.serve_forever()

if __name__=='__main__':
    import sys
    serve(json.loads(Path(sys.argv[1]).read_text()))
