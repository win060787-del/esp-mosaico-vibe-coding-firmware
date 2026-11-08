"""Private single-device snapshot relay. Credentials stay outside source."""
import hmac
import json
import os
import re
import sqlite3
import sys
from urllib.parse import urlsplit,parse_qs
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parent))
from pet_sources import MultiStore,source_name

MAX_BODY = 4096

def validate(data):
    def integer(value, low, high):
        return type(value) is int and low <= value <= high
    def title(value):
        return isinstance(value, str) and len(value.encode()) <= 96 and all(ord(c) >= 32 for c in value)
    if not isinstance(data, dict) or data.get('v') != 1:
        raise ValueError('invalid schema')
    if not integer(data.get('ts'), int(time.time())-90, int(time.time())+30):
        raise ValueError('invalid timestamp')
    if not integer(data.get('active'), 0, 9999) or data.get('state') not in ('offline','idle','working','waiting','completed','failed'):
        raise ValueError('invalid state')
    if type(data.get('ok')) is not bool or type(data.get('partial')) is not bool:
        raise ValueError('invalid flags')
    windows=data.get('windows');tasks=data.get('tasks');done=data.get('done')
    if not isinstance(windows,list) or len(windows)>2 or not isinstance(tasks,list) or len(tasks)>4:
        raise ValueError('invalid lists')
    for w in windows:
        if not isinstance(w,dict) or not integer(w.get('remaining'),0,100) or not integer(w.get('reset_at'),0,4294967295) or not title(w.get('label')):
            raise ValueError('invalid quota')
    for task in tasks:
        if not isinstance(task,dict) or type(task.get('unread',False)) is not bool or not title(task.get('title')) or task.get('state','unknown') not in ('working','waiting','idle','completed','failed','unknown'):
            raise ValueError('invalid task')
    if done is not None and (not isinstance(done,dict) or not title(done.get('title')) or not integer(done.get('at'),1,data['ts'])):
        raise ValueError('invalid completion')
    if not integer(data.get('done_count',0),0,8):raise ValueError('invalid completion count')
    completions=data.get('completions',[])
    if not isinstance(completions,list) or len(completions)>8:raise ValueError('invalid completion events')
    for event in completions:
        if not isinstance(event,dict) or not isinstance(event.get('key'),str) or not re.fullmatch('[0-9a-f]{64}',event['key']) or not integer(event.get('at'),max(1,data['ts']-600),data['ts']):
            raise ValueError('invalid completion event')
    # Persist only the device contract; never arbitrary collector fields.
    result={k:data[k] for k in ('v','ts','state','active','partial','windows','ok','tasks','done')}
    account=data.get('account_hash','')
    if not isinstance(account,str) or (account and not re.fullmatch('[a-f0-9]{64}',account)):raise ValueError('invalid account')
    if 'source_name' in data and not source_name(data['source_name']):raise ValueError('invalid name')
    result['account_hash']=account
    if 'source_name' in data:result['source_name']=data['source_name']
    recent=data.get('recent')
    if recent is not None and (not isinstance(recent,dict) or not title(recent.get('title')) or not integer(recent.get('at'),1,data['ts'])):raise ValueError('invalid recent interaction')
    result['recent']={'title':recent['title'],'at':recent['at']} if recent else None
    result['done_count']=data.get('done_count',0)
    result['completions']=sorted([{'key':e['key'],'at':e['at']} for e in completions],key=lambda e:e['at'])
    result['tasks']=[]
    for task in tasks:
        item={'title':task['title'],'state':task.get('state','unknown'),'unread':task.get('unread',False)}
        if 'key' in task:
            if not isinstance(task['key'],str) or not re.fullmatch('[a-f0-9]{64}',task['key']):raise ValueError('invalid task key')
            item['key']=task['key']
        result['tasks'].append(item)
    return result

class Store(MultiStore):
    pass

class Handler(BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def log_message(self,*args):pass
    def reply(self,status,data):
        body=json.dumps(data,ensure_ascii=False,separators=(',',':')).encode()
        self.send_response(status);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(body)
    def authorized(self,role):
        expected=self.server.tokens.get(role)
        return bool(expected) and hmac.compare_digest(self.headers.get('Authorization','').encode(),('Bearer '+expected).encode())
    def read_body(self):
        length=int(self.headers.get('Content-Length','0'))
        if not 0<length<=MAX_BODY:raise ValueError('invalid length')
        body=json.loads(self.rfile.read(length))
        if not isinstance(body,dict):raise ValueError('invalid body')
        return body
    def do_GET(self):
        parts=urlsplit(self.path);query=parse_qs(parts.query)
        if parts.path=='/healthz':return self.reply(200,{'ok':True})
        if not self.authorized('read'):return self.reply(401,{'error':'unauthorized'})
        try:
            mode=query.get('mode',['auto'])[0];fixed=query.get('terminal',[''])[0];viewer=query.get('viewer',['legacy'])[0]
            if parts.path=='/v1/terminals':
                page=int(query.get('page',['0'])[0])
                if not 0<=page<16:raise ValueError()
                return self.reply(200,self.server.store.catalog(page,mode,fixed,viewer))
            if parts.path!='/v1/state':return self.reply(404,{'error':'not found'})
            data=self.server.store.get(mode,fixed,viewer)
            self.reply(200 if data else 503,data or {'error':'no snapshot'})
        except (ValueError,TypeError):self.reply(400,{'error':'invalid selection'})
    def do_PUT(self):
        if self.path!='/v1/state':return self.reply(404,{'error':'not found'})
        token=self.headers.get('Authorization','')
        terminal='legacy' if self.authorized('write') else self.server.store.writer(token[7:]) if token.startswith('Bearer ') else None
        if not terminal:return self.reply(401,{'error':'unauthorized'})
        try:
            data=validate(self.read_body());self.server.store.put(data,terminal)
        except (ValueError,KeyError,TypeError):return self.reply(400,{'error':'invalid snapshot'})
        self.reply(200,{'ok':True,'ts':data['ts']})
    def do_POST(self):
        try:
            if self.path=='/v1/enroll-code':
                if not self.authorized('manage'):return self.reply(401,{'error':'unauthorized'})
                self.read_body()
                return self.reply(200,self.server.store.enrollment_code())
            if self.path=='/v1/enroll':
                body=self.read_body();result=self.server.store.enroll(body.get('code'),body.get('source_id'),body.get('name'))
                return self.reply(200,result)
            if self.path=='/v1/owner/register':
                if not self.authorized('manage'):return self.reply(401,{'error':'unauthorized'})
                body=self.read_body();return self.reply(200,self.server.store.register(body.get('source_id'),body.get('name')))
            self.reply(404,{'error':'not found'})
        except (ValueError,KeyError,TypeError,sqlite3.IntegrityError):self.reply(400,{'error':'invalid or expired pairing'})

def main():
    config=json.loads(Path(os.environ.get('PET_CONFIG','/run/pet/config.json')).read_text())
    server=ThreadingHTTPServer(('127.0.0.1',int(os.environ.get('PET_PORT','18765'))),Handler)
    server.tokens=config;server.store=Store(os.environ.get('PET_DB','/data/pet.db'));server.serve_forever()

if __name__=='__main__':main()
