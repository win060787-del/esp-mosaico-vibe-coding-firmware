"""Normalize real Codex quotas and local turn boundaries; never export chat text."""
import hashlib
import json
import math
import os
import time
from pathlib import Path
from .read_state import unread_thread_ids
from datetime import datetime

RECENT_SECONDS = 600


def event_time(record):
    try:
        return int(datetime.fromisoformat(record['timestamp'].replace('Z', '+00:00')).timestamp())
    except (KeyError, ValueError, TypeError):
        return 0


# Per-file cursor avoids repeatedly parsing multi-megabyte transcript tails.
# Values are private collector state; no transcript content is retained or exported.
_ACTIVITY_CACHE = {}

def _empty_activity():
    return {'state':'unknown','completed_at':0,'turn_id':'','step':'','failed_at':0,'completions':[],'last_at':0}


def _consume_activity(entry, raw):
    # Most records are messages; only lifecycle/tool envelopes affect this adapter.
    if not any(marker in raw for marker in (b'"event_msg"',b'"function_call"',b'"function_call_output"')):
        return
    try:
        record=json.loads(raw)
    except (ValueError,UnicodeError):
        return
    payload=record.get('payload') or {}
    kind=payload.get('type')
    result=entry['activity']
    result['last_at']=max(result.get('last_at',0),event_time(record))
    if record.get('type')=='event_msg':
        if kind=='task_started':
            result.update(state='working',turn_id=payload.get('turn_id',''),step='')
            entry['waiting_call']=None
        elif kind in ('task_complete','turn_aborted','task_failed'):
            result['state']='failed' if kind=='task_failed' else 'idle'
            entry['waiting_call']=None
            if kind=='task_failed':result['failed_at']=event_time(record)
            if kind=='task_complete':
                result['completed_at']=event_time(record)
                result['turn_id']=payload.get('turn_id',result['turn_id'])
                result['completions'].append({'at':result['completed_at'],'turn_id':result['turn_id']})
                result['completions']=result['completions'][-16:]
    elif record.get('type')=='response_item' and result['state'] in ('working','waiting'):
        if kind=='function_call':
            name=payload.get('name','')
            if 'request_user_input' in name:
                result['state']='waiting';entry['waiting_call']=payload.get('call_id')
            elif name.endswith('update_plan'):
                try:
                    plan=json.loads(payload.get('arguments','{}')).get('plan',[])
                    result['step']=next((x['step'] for x in plan if x.get('status')=='in_progress'),'')[:60]
                except (ValueError,TypeError,KeyError):pass
        elif kind=='function_call_output' and result['state']=='waiting' and payload.get('call_id')==entry['waiting_call']:
            result['state']='working';entry['waiting_call']=None


def read_activity(thread, now):
    """Index a file once, then consume only appended complete records.

    A long tool result must not hide an earlier task_started boundary. File
    replacement/truncation invalidates the cursor, and partial lines are retried.
    """
    path=thread.get('path')
    if not path:
        result=_empty_activity()
        result['state']='working' if (thread.get('status') or {}).get('type')=='active' else 'unknown'
        return result
    try:
        with open(path,'rb') as f:
            stat=os.fstat(f.fileno())
            signature=(stat.st_dev,stat.st_ino,stat.st_size,stat.st_mtime_ns,stat.st_ctime_ns)
            entry=_ACTIVITY_CACHE.get(path)
            if not entry or entry['signature']!=signature:
                resume=bool(entry and entry['signature'][:2]==signature[:2] and stat.st_size>=entry['signature'][2])
                if resume:
                    f.seek(max(0,entry['offset']-64))
                    resume=f.read(min(64,entry['offset']))==entry['anchor']
                if not resume:
                    entry={'activity':_empty_activity(),'waiting_call':None,'offset':0}
                f.seek(entry['offset'])
                while True:
                    offset=f.tell();raw=f.readline()
                    if not raw or not raw.endswith(b'\n'):
                        entry['offset']=offset;break
                    _consume_activity(entry,raw)
                f.seek(max(0,entry['offset']-64))
                entry['anchor']=f.read(min(64,entry['offset']))
                entry['signature']=signature
                _ACTIVITY_CACHE[path]=entry
                if len(_ACTIVITY_CACHE)>512:_ACTIVITY_CACHE.pop(next(iter(_ACTIVITY_CACHE)))
    except OSError:
        _ACTIVITY_CACHE.pop(path,None)
        return _empty_activity()
    result={**entry['activity'],'completions':[dict(e) for e in entry['activity']['completions'] if 0<=now-e['at']<RECENT_SECONDS]}
    if result['state'] in ('working','waiting') and now-stat.st_mtime>7200:result['state']='unknown'
    return result


def quota_windows(result):
    buckets = result.get('rateLimitsByLimitId') or {}
    limits = buckets.get('codex') or result.get('rateLimits') or {}
    windows = []
    for name in ('primary', 'secondary'):
        window = limits.get(name)
        if not isinstance(window, dict):
            continue
        used, duration, reset = (window.get(k) for k in ('usedPercent', 'windowDurationMins', 'resetsAt'))
        if not isinstance(used, (float, int)) or not math.isfinite(used):
            continue
        if not isinstance(duration, int) or not isinstance(reset, (int, float)):
            continue
        label = '本周剩余' if duration == 10080 else ('5小时剩余' if duration == 300 else '额度剩余')
        windows.append({'label': label, 'remaining': max(0, min(100, round(100 - used))),
                        'reset_at': int(reset), 'minutes': duration})
    return windows


def make_snapshot(rate_limits, threads, now=None, truncated=False, unread_ids=None):
    now = int(time.time()) if now is None else now
    unread_ids=unread_thread_ids() if unread_ids is None else unread_ids
    tasks = []
    unread_tasks = []
    completions = {}
    latest = None
    recent = None
    failed_at = 0
    for thread in threads:
        activity = read_activity(thread, now)
        for event in activity['completions']:
            identity=json.dumps([thread.get('id',''),event['turn_id'],event['at']],separators=(',',':'))
            key=hashlib.sha256(identity.encode()).hexdigest()
            completions[key]={'key':key,'at':event['at']}
        title = str(thread.get('name') or Path(thread.get('cwd') or 'Codex').name or 'Codex')[:40]
        item = {'id': thread.get('id', ''), 'title': title, **activity}
        item['unread']=item['id'] in unread_ids
        at=max(activity.get('last_at',0),int(thread.get('updatedAt') or 0))
        if 0<at<=now and (recent is None or at>recent['at']):
            label={'working':'执行中','waiting':'待确认','idle':'已结束','failed':'失败','unknown':'最近查看'}.get(item['state'],'最近交互')
            detail=item.get('step') or title
            recent={'at':at,'title':(label+' · '+detail).encode()[:96].decode('utf-8',errors='ignore')}

        if item['state'] in ('working', 'waiting'):
            tasks.append(item)
        elif item['unread'] and item['state']=='idle' and item['completed_at'] and item['completed_at']>=item['failed_at']:
            unread_tasks.append({**item,'state':'completed'})
        failed_at = max(failed_at, item['failed_at'])
        at = item['completed_at']
        if 0 <= now - at < RECENT_SECONDS and (latest is None or at > latest['completed_at']):
            latest = item
    tasks.sort(key=lambda x: x['state'] != 'waiting')
    state = 'waiting' if any(t['state'] == 'waiting' for t in tasks) else ('working' if tasks else 'idle')
    if not tasks:
        if 0 <= now - failed_at < RECENT_SECONDS and (latest is None or failed_at > latest['completed_at']):
            state = 'failed'
        elif latest:
            state = 'completed'
    return {'v': 1, 'generated_at': now, 'state': state, 'windows': quota_windows(rate_limits),
            'active': len(tasks), 'count_partial': truncated, 'tasks': (tasks+sorted(unread_tasks,key=lambda x:x['completed_at'],reverse=True))[:8],
            'just_completed': latest, 'recent':recent, 'source_ok': True, 'completions': sorted(completions.values(),key=lambda e:e['at'])[-8:]}
