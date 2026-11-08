import json
import os
from datetime import datetime, timezone
from mosaico_pet.state import make_snapshot, read_activity
from mosaico_pet.main import compact

NOW = 1793547730

def record(kind, **extra):
    return {'timestamp': datetime.fromtimestamp(NOW-30,timezone.utc).isoformat(),
            'type':'event_msg','payload':{'type':kind,'turn_id':'turn-1',**extra}}

def thread(tmp_path, records):
    p=tmp_path/'rollout.jsonl'
    p.write_text('\n'.join(json.dumps(r) for r in records)+'\n')
    os.utime(p,(NOW,NOW))
    return {'id':'thread-1','name':'开发桌宠','path':str(p),'status':{'type':'notLoaded'}}

def test_real_lifecycle_overrides_other_app_server_not_loaded(tmp_path):
    t=thread(tmp_path,[record('task_started')])
    s=make_snapshot({},[t],NOW)
    assert s['active']==1 and s['state']=='working'
    assert s['windows']==[]  # unavailable is not a fabricated 100% allowance

def test_completion_while_other_task_keeps_running(tmp_path):
    completed=thread(tmp_path,[record('task_started'),record('task_complete')])
    active={'id':'thread-2','name':'应用接口','status':{'type':'active'}}
    s=make_snapshot({},[completed,active],NOW)
    assert s['active']==1 and s['state']=='working'
    assert s['just_completed']['title']=='开发桌宠'
    assert compact(s)['done']['at']==NOW-30
    assert 'path' not in json.dumps(compact(s))

def test_expired_completion_is_not_just_completed(tmp_path):
    t=thread(tmp_path,[record('task_complete')])
    s=make_snapshot({},[t],NOW+601)
    assert s['just_completed'] is None

def test_waiting_only_ends_for_matching_input(tmp_path):
    call={'type':'response_item','payload':{'type':'function_call','name':'request_user_input','call_id':'ask'}}
    unrelated={'type':'response_item','payload':{'type':'function_call_output','call_id':'other'}}
    t=thread(tmp_path,[record('task_started'),call,unrelated])
    assert read_activity(t,NOW)['state']=='waiting'
    matching={'type':'response_item','payload':{'type':'function_call_output','call_id':'ask'}}
    t=thread(tmp_path,[record('task_started'),call,matching])
    assert read_activity(t,NOW)['state']=='working'

def test_abandoned_turn_and_broken_tail_do_not_report_live_work(tmp_path):
    t=thread(tmp_path,[record('task_started')])
    assert read_activity(t,NOW+7201)['state']=='unknown'
    with open(t['path'],'a') as f:f.write('{partial')
    assert read_activity(t,NOW)['state']=='working'

def test_weekly_quota_and_reset_are_account_values():
    s=make_snapshot({'rateLimits':{'primary':{'usedPercent':57,'windowDurationMins':10080,'resetsAt':NOW+200000}}},[],NOW)
    assert s['windows']==[{'label':'本周剩余','remaining':43,'minutes':10080,'reset_at':NOW+200000}]

def test_recent_failed_turn_drives_failure_expression(tmp_path):
    t=thread(tmp_path,[record('task_started'),record('task_failed')])
    assert make_snapshot({},[t],NOW)['state']=='failed'

def test_manually_stopped_turn_is_not_a_failure(tmp_path):
    t=thread(tmp_path,[record('task_started'),record('turn_aborted')])
    s=make_snapshot({},[t],NOW)
    assert s['state']=='idle' and s['active']==0 and s['just_completed'] is None


def test_unread_completion_is_listed_until_read_and_not_counted_active(tmp_path):
    t=thread(tmp_path,[record('task_started'),record('task_complete')])
    unread=make_snapshot({},[t],NOW,unread_ids={'thread-1'})
    assert unread['active']==0
    assert unread['tasks'][0]['state']=='completed' and unread['tasks'][0]['unread']
    assert compact(unread)['tasks'][0]['unread'] is True
    read=make_snapshot({},[t],NOW,unread_ids=set())
    assert read['tasks']==[]
    assert read['just_completed']['title']=='开发桌宠'


def test_desktop_sidebar_state_overrides_historical_receipts_without_writing(tmp_path):
    import sqlite3
    from mosaico_pet.read_state import unread_thread_ids
    db=sqlite3.connect(tmp_path/'state_5.sqlite')
    db.execute('create table thread_read_receipts(thread_id text,first_unread_turn text)')
    db.executemany('insert into thread_read_receipts values(?,?)',[('read',None),('unread','turn')]);db.commit();db.close()
    (tmp_path/'.codex-global-state.json').write_text(json.dumps({'electron-thread-read-state-v1':{'unreadByIdentity':{'account':{'local:host':['read','legacy'],'durable:host':['remote']}}}}))
    before=(tmp_path/'state_5.sqlite').read_bytes()
    assert unread_thread_ids(tmp_path)=={'read','legacy'}
    (tmp_path/'.codex-global-state.json').write_text(json.dumps({'electron-thread-read-state-v1':{'unreadByIdentity':{'account':{'local:host':[]}}}}))
    assert unread_thread_ids(tmp_path)==set()
    assert (tmp_path/'state_5.sqlite').read_bytes()==before


def test_completion_keys_survive_renames_and_next_turn(tmp_path):
    t=thread(tmp_path,[record('task_started'),record('task_complete')])
    first=make_snapshot({},[t],NOW,unread_ids=set())
    t['name']='重命名任务'
    with open(t['path'],'a') as f:f.write(json.dumps(record('task_started',turn_id='next-turn'))+'\n')
    second=make_snapshot({},[t],NOW,unread_ids=set())
    assert first['completions']==second['completions']
    assert len(compact(second)['completions'][0]['key'])==64


def test_all_completion_events_are_kept_even_between_polls(tmp_path):
    t=thread(tmp_path,[record('task_started'),record('task_complete'),record('task_started',turn_id='second'),record('task_complete',turn_id='second')])
    s=make_snapshot({},[t],NOW,unread_ids=set())
    assert len(s['completions'])==2
    assert len({event['key'] for event in s['completions']})==2


def test_long_running_turn_is_not_lost_after_large_tool_output(tmp_path):
    t=thread(tmp_path,[record('task_started')])
    with open(t['path'],'a') as f:
        f.write(json.dumps({'type':'response_item','payload':{'type':'function_call_output','output':'x'*(3*1024*1024)}})+'\n')
    first=make_snapshot({},[t],NOW,unread_ids=set())
    assert first['active']==1 and first['tasks'][0]['state']=='working'
    with open(t['path'],'a') as f:f.write(json.dumps(record('task_complete'))+'\n')
    second=make_snapshot({},[t],NOW,unread_ids=set())
    assert second['active']==0 and second['just_completed']['title']=='开发桌宠'


def test_incremental_reader_waits_for_complete_line_and_detects_rewrite(tmp_path):
    t=thread(tmp_path,[record('task_started')])
    assert read_activity(t,NOW)['state']=='working'
    with open(t['path'],'a') as f:f.write(json.dumps(record('task_complete')))
    assert read_activity(t,NOW)['state']=='working'
    with open(t['path'],'a') as f:f.write('\n')
    assert read_activity(t,NOW)['state']=='idle'
    replacement=thread(tmp_path,[record('task_failed')])
    activity=read_activity(replacement,NOW)
    assert activity['state']=='failed' and activity['completed_at']==0


def test_compact_counts_only_simultaneous_recent_completions():
 s={'generated_at':NOW,'state':'completed','active':0,'count_partial':False,'windows':[],'source_ok':True,'tasks':[],
    'just_completed':{'title':'最新任务','completed_at':NOW},
    'completions':[{'key':'a'*64,'at':NOW},{'key':'b'*64,'at':NOW-1},{'key':'c'*64,'at':NOW-10}]}
 assert compact(s)['done_count']==2

def test_recent_interaction_survives_completion_expiry(tmp_path):
 t=thread(tmp_path,[record('task_complete')])
 s=make_snapshot({},[t],NOW+601,unread_ids=set())
 assert s['just_completed'] is None
 assert compact(s)['recent']['at']==NOW-30
 assert '开发桌宠' in compact(s)['recent']['title']

def test_recent_interaction_tracks_latest_task(tmp_path):
 old=thread(tmp_path,[record('task_complete')])
 new={'id':'new','name':'更新界面','updatedAt':NOW-1,'status':{'type':'active'}}
 assert make_snapshot({},[old,new],NOW,unread_ids=set())['recent']['title']=='执行中 · 更新界面'
