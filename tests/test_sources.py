import json,sqlite3,time
from pathlib import Path
import pytest
import sys
sys.path.insert(0,str(Path(__file__).parents[1]/'server'))
from pet_sources import MultiStore

def state(active=0,account='a'*64,task='1'*64):
 now=int(time.time())
 return {'v':1,'ts':now,'state':'working' if active else 'idle','active':active,'ok':True,'partial':False,'windows':[{'label':'本周剩余','remaining':50,'reset_at':now+3600}], 'tasks':[{'title':'真实任务','state':'working','key':task,'unread':False}] if active else [],'done':None,'completions':[],'account_hash':account}

def test_auto_follows_work_but_stays_on_current_when_both_work(tmp_path):
 s=MultiStore(str(tmp_path/'db'));a='a'*32;b='b'*32
 s.register(a,'Mac mini');s.register(b,'MacBook')
 s.put(state(),a);s.put(state(1),b)
 assert s.get()['source']['id']==b
 s.put(state(1),a)
 assert s.get()['source']['id']==b
 s.put(state(),b)
 assert s.get()['source']['id']==a
 assert s.get('fixed',b)['source']['id']==b
 with sqlite3.connect(s.path) as db:
  d=state();d['ts']=int(time.time())-100
  db.execute('UPDATE terminal_state SET ts=?,body=? WHERE id=?',(d['ts'],json.dumps(d),b))
 assert s.get('fixed',b)['ok'] is False
 assert s.get()['source']['id']==a

def test_aggregate_deduplicates_shared_chat_and_never_sums_account_quota(tmp_path):
 s=MultiStore(str(tmp_path/'db'));a='a'*32;b='b'*32
 s.register(a,'Mac mini');s.register(b,'MacBook')
 s.put(state(1),a);s.put(state(1),b)
 result=s.get('all');assert result['active']==1 and result['windows'][0]['remaining']==50
 other=state(1,account='b'*64,task='2'*64);s.put(other,b)
 result=s.get('all');assert result['active']==2 and result['windows']==[] and result['quota_label']=='按终端查看'
 assert all(' · ' in t['title'] for t in result['tasks'])

def test_enrollment_is_one_use_and_writer_is_bound_to_terminal(tmp_path):
 s=MultiStore(str(tmp_path/'db'));code=s.enrollment_code();a='a'*32
 credential=s.enroll(code['code'],a,'Mac mini')
 assert s.writer(credential['write'])==a and s.writer('invalid') is None
 with pytest.raises(ValueError):s.enroll(code['code'],'b'*32,'MacBook')
 expired=s.enrollment_code()
 with sqlite3.connect(s.path) as db:db.execute('UPDATE enroll_codes SET expires=0')
 with pytest.raises(ValueError):s.enroll(expired['code'],'c'*32,'Office')

def test_catalog_pagination_and_metadata_never_leak_credentials(tmp_path):
 s=MultiStore(str(tmp_path/'db'))
 for i in range(7):s.register(f'{i:032x}','电脑'+str(i))
 assert len(s.catalog(0)['terminals'])==4 and len(s.catalog(1)['terminals'])==3
 assert s.catalog()['total']==7
 assert 'writer' not in json.dumps(s.catalog())
