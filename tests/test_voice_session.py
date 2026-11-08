import importlib.util
from pathlib import Path
import pytest
spec=importlib.util.spec_from_file_location('voice',Path(__file__).parents[1]/'companion/voice/service.py')
voice=importlib.util.module_from_spec(spec);spec.loader.exec_module(voice)
class Engine:
 def __init__(self):self.actions=[];self.result='你好';self.fail=False
 def warm(self):pass
 def transcribe(self,pcm):return self.result
 def edit(self,action,session,text=''):
  if self.fail:raise RuntimeError('focus changed')
  self.actions.append((action,text))
def command(s,action,key=None):
 s.command(dict(action=action,device='pet',request=key or str(len(s.requests))));s.step()
def recorded():
 e=Engine();s=voice.Session(e,'Test Mac');command(s,'start');s.audio('pet',0,b'\x01\x00'*1600);return s,e
def test_start_requires_editor_before_recording():
 s,e=recorded();assert s.state=='recording';assert e.actions[0][0]=='begin'
def test_duplicate_audio_and_command():
 s,e=recorded();s.audio('pet',0,b'\x01\x00'*1600);assert len(s.pcm)==3200
 command(s,'stop','stop');command(s,'stop','stop');assert e.actions.count(('update','你好'))==1
 assert s.text=='你好' and s.state=='ready'
def test_stop_then_append_preserves_draft():
 s,e=recorded();command(s,'stop');command(s,'start');s.audio('pet',0,b'\x01\x00'*1600);e.result='世界';command(s,'stop')
 assert s.text=='你好世界';assert sum(a=='begin' for a,t in e.actions)==1
def test_send_finishes_and_deduplicates():
 s,e=recorded();command(s,'send','send');command(s,'send','send')
 assert e.actions[-2:]==[('update','你好'),('send','')];assert s.state=='sent'
def test_delete_pauses_and_handles_graphemes():
 s,e=recorded();e.result='你好👨‍👩‍👧';command(s,'delete');assert s.text=='你好' and s.state=='ready'
 command(s,'clear');assert s.text=='' and e.actions[-1]==('update','')
def test_focus_failure_does_not_send():
 s,e=recorded();e.fail=True
 with pytest.raises(RuntimeError):command(s,'send')
 assert not any(a=='send' for a,t in e.actions)
def test_rejects_other_device_and_out_of_order():
 s,e=recorded()
 with pytest.raises(ValueError):s.audio('other',1,b'00')
 with pytest.raises(ValueError):s.audio('pet',9,b'00')
 with pytest.raises(ValueError):s.command(dict(action='send',device='other',request='x'))
def test_uncertain_send_never_retries():
 s,e=recorded();command(s,'stop');e.fail=True
 with pytest.raises(RuntimeError):command(s,'send')
 e.fail=False
 with pytest.raises(RuntimeError):command(s,'send')
 assert not any(a=='send' for a,t in e.actions)

def test_batched_audio_preserves_samples_and_flush_tail():
 s,e=recorded()
 batch=b'\x02\x00'*6400;tail=b'\x03\x00'*1600
 s.audio('pet',1,batch);s.audio('pet',2,tail)
 assert bytes(s.pcm)==b'\x01\x00'*1600+batch+tail
 command(s,'stop');assert s.state=='ready' and s.text=='你好'
 with pytest.raises(ValueError):s.audio('pet',3,b'00'*10000)

def test_restart_after_edit_failure_preserves_mac_draft_as_new_session():
 s,e=recorded();command(s,'stop');old=s.session
 s.change('error','输入框已被手动编辑，已停止流式改写')
 command(s,'start')
 assert s.state=='recording' and s.session!=old and s.text==''
 assert e.actions[-2:]==[('end',''),('begin','')]

def test_uncertain_send_blocks_restart():
 s,e=recorded();command(s,'stop');s.sending=True;s.change('error','uncertain')
 with pytest.raises(RuntimeError):command(s,'start')
 assert s.sending
