import copy
import importlib.util
import json
from pathlib import Path
import threading
import time
import urllib.request
import urllib.error
from http.server import ThreadingHTTPServer
import pytest

spec=importlib.util.spec_from_file_location('pet_server',Path(__file__).parents[1]/'server/pet_server.py')
relay=importlib.util.module_from_spec(spec);spec.loader.exec_module(relay)

def snapshot():
 return {'v':1,'ts':int(time.time()),'active':2,'state':'working','ok':True,'partial':False,'windows':[{'label':'本周剩余','remaining':35,'reset_at':int(time.time())+86400}], 'tasks':[{'title':'开发任务'}],'done':None}

def test_persist_order_and_stale(tmp_path,monkeypatch):
 store=relay.Store(str(tmp_path/'state.db'));data=snapshot();store.put(relay.validate(data))
 older=copy.deepcopy(data);older.update(ts=data['ts']-1,active=99);store.put(older)
 assert relay.Store(str(tmp_path/'state.db')).get()['active']==2
 monkeypatch.setattr(relay.time,'time',lambda:data['ts']+91)
 assert store.get()['ok'] is False
 assert store.get()['state']=='offline'

@pytest.mark.parametrize('mutation',[lambda d:d.update(active=True),lambda d:d.update(ts=0),lambda d:d.update(tasks=[{'title':'a'*97}]),lambda d:d.update(windows=[{'label':'week','remaining':101,'reset_at':0}])])
def test_reject_invalid(mutation):
 data=snapshot();mutation(data)
 with pytest.raises(ValueError):relay.validate(data)

def test_read_write_credentials_separate(tmp_path):
 server=ThreadingHTTPServer(('127.0.0.1',0),relay.Handler);server.store=relay.Store(str(tmp_path/'state.db'));server.tokens={'read':'reader','write':'writer'}
 thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
 url='http://127.0.0.1:'+str(server.server_port)+'/v1/state'
 def call(method,token,data=None):
  req=urllib.request.Request(url,data=json.dumps(data).encode() if data else None,method=method,headers={'Authorization':'Bearer '+token})
  return urllib.request.urlopen(req,timeout=2)
 try:
  for method,token,data in [('PUT','reader',snapshot()),('GET','writer',None),('GET','wrong',None)]:
   with pytest.raises(urllib.error.HTTPError) as exc:call(method,token,data)
   assert exc.value.code==401
  with call('PUT','writer',snapshot()) as r:assert json.load(r)['ok']
  with call('GET','reader') as r:assert json.load(r)['active']==2
 finally:server.shutdown();server.server_close();thread.join()


def test_server_clock_independent_of_mac_and_task_states(tmp_path,monkeypatch):
 data=snapshot();data['server_time']=1
 data['tasks']=[{'title':'进行中','state':'working'},{'title':'等待确认','state':'waiting'}]
 store=relay.Store(str(tmp_path/'clock.db'));store.put(relay.validate(data))
 monkeypatch.setattr(relay.time,'time',lambda:data['ts']+25)
 result=store.get()
 assert result['ts']==data['ts']
 assert result['server_time']==data['ts']+25
 assert [t['state'] for t in result['tasks']]==['working','waiting']
 data['tasks'][0]['state']='invented'
 with pytest.raises(ValueError):relay.validate(data)


def test_completion_event_validation_and_order():
 data=snapshot();data['completions']=[{'key':'b'*64,'at':data['ts']},{'key':'a'*64,'at':data['ts']-1}]
 normalized=relay.validate(data)
 assert [e['key'] for e in normalized['completions']]==['a'*64,'b'*64]
 data['completions'][0]['key']='not-an-event-key'
 with pytest.raises(ValueError):relay.validate(data)


def test_completion_count_and_persistent_http_connection(tmp_path):
 import http.client
 server=ThreadingHTTPServer(('127.0.0.1',0),relay.Handler)
 server.store=relay.Store(str(tmp_path/'keepalive.db'));server.tokens={'read':'reader','write':'writer'}
 data=snapshot();data['done_count']=3;data['done']={'title':'完成任务','at':data['ts']};data['completions']=[{'key':str(i)*64,'at':data['ts']} for i in range(3)]
 server.store.put(relay.validate(data))
 thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
 conn=http.client.HTTPConnection('127.0.0.1',server.server_port,timeout=2)
 try:
  conn.request('GET','/v1/state',headers={'Authorization':'Bearer reader'})
  first=conn.getresponse();assert json.loads(first.read())['done_count']==3
  sock=conn.sock;assert sock is not None and not first.will_close
  conn.request('GET','/v1/state',headers={'Authorization':'Bearer reader'})
  second=conn.getresponse();assert second.status==200;second.read()
  assert conn.sock is sock
  data['done_count']=9
  with pytest.raises(ValueError):relay.validate(data)
 finally:conn.close();server.shutdown();server.server_close();thread.join()


def test_enrollment_http_roles_and_terminal_identity(tmp_path):
 server=ThreadingHTTPServer(('127.0.0.1',0),relay.Handler)
 server.store=relay.Store(str(tmp_path/'enrollment.db'));server.tokens={'read':'reader','write':'legacywriter','manage':'owner'}
 thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
 base='http://127.0.0.1:'+str(server.server_port)
 def call(path,token='',body=None,method=None):
  request=urllib.request.Request(base+path,data=json.dumps(body).encode() if body is not None else None,method=method,headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
  with urllib.request.urlopen(request,timeout=2) as result:return json.load(result)
 try:
  with pytest.raises(urllib.error.HTTPError) as exc:call('/v1/enroll-code','reader',{})
  assert exc.value.code==401
  code=call('/v1/enroll-code','owner',{})['code']
  a=call('/v1/enroll',body={'code':code,'source_id':'a'*32,'name':'MacBook'})
  assert server.store.writer(a['write'])=='a'*32
  with pytest.raises(urllib.error.HTTPError):call('/v1/enroll',body={'code':code,'source_id':'b'*32,'name':'Other'})
  data=snapshot();data['source_id']='b'*32
  call('/v1/state',a['write'],data,'PUT')
  rows=call('/v1/terminals','reader')['terminals']
  assert len(rows)==1 and rows[0]['id']=='a'*32 and rows[0]['online']
  with pytest.raises(urllib.error.HTTPError):call('/v1/state',a['write'])
  old=call('/v1/enroll-code','owner',{})['code'];call('/v1/enroll-code','owner',{})
  with pytest.raises(urllib.error.HTTPError):call('/v1/enroll',body={'code':old,'source_id':'c'*32,'name':'Old code'})
 finally:server.shutdown();server.server_close();thread.join()
