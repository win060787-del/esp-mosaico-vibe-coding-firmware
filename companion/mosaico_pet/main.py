"""Mac data companion; all device traffic goes through the official Mosaico CLI."""
import argparse
import hashlib
import socket
import asyncio
import json
import logging
import os
import fcntl
import sys
import time
import ssl
import urllib.request
from pathlib import Path
from .app_server import AppServerClient
from .state import make_snapshot

ROOT = Path(__file__).resolve().parents[2]
WORKSPACE = ROOT / 'vendor/esp-mosaico-vibe-legacy-v26.0.1'
PROJECT = 'projects/codex_pet'
SCAN_SECONDS = 0.5
THREAD_LIST_SECONDS = 2
HEARTBEAT_SECONDS = 10
UPLOAD_RETRY_SECONDS = 2
CACHE = Path.home() / 'Library/Caches/MosaicoCodexPet/state.json'


async def fetch_threads(client):
    threads, cursor = [], None
    for _ in range(5):
        params = {'limit': 100, 'sortKey': 'recency_at', 'sortDirection': 'desc',
                  'archived': False, 'useStateDbOnly': True}
        if cursor:
            params['cursor'] = cursor
        result = await client.request('thread/list', params)
        threads.extend(result.get('data') or [])
        cursor = result.get('nextCursor')
        if not cursor:
            break
    return threads, bool(cursor)


def compact(snapshot):
    """Bound the USB RPC payload and omit local paths, IDs and transcript contents."""
    completed = snapshot['just_completed']
    return {'v': 1, 'ts': snapshot['generated_at'], 'state': snapshot['state'],
            'active': snapshot['active'], 'partial': snapshot['count_partial'],
            'done_count': sum(1 for e in snapshot.get('completions',[]) if completed and completed['completed_at']-1<=e['at']<=completed['completed_at']),
            'windows': snapshot['windows'], 'ok': snapshot['source_ok'],
            'completions': snapshot.get('completions',[]),
            'tasks': [{'title': t['title'], 'state': t['state'], 'unread': t.get('unread',False), **({'key':hashlib.sha256(str(t['id']).encode()).hexdigest()} if t.get('id') else {})} for t in snapshot['tasks'][:4]],
            'account_hash':snapshot.get('account_hash',''),
            'recent':snapshot.get('recent'),
            'done': {'title': completed['title'], 'at': completed['completed_at']} if completed else None}


def save(snapshot):
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    temporary = CACHE.with_suffix('.tmp')
    temporary.write_text(json.dumps(snapshot, ensure_ascii=False), encoding='utf-8')
    temporary.chmod(0o600)
    temporary.replace(CACHE)


async def push_usb(snapshot, device_id):
    args = [sys.executable, str(WORKSPACE / 'mosaico.py'), 'iris', 'rpc', '0x70', '1',
            '--project', PROJECT, '--payload', json.dumps(compact(snapshot), ensure_ascii=False, separators=(',', ':'))]
    if device_id:
        args += ['--device-id', device_id]
    process = await asyncio.create_subprocess_exec(*args, cwd=WORKSPACE,
              stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    try:
        stdout, stderr = await asyncio.wait_for(process.communicate(), 15)
    except asyncio.TimeoutError:
        process.kill()
        await process.wait()
        raise RuntimeError('Device update timed out')
    if process.returncode:
        # Avoid persisting raw gateway output or any credentials.
        raise RuntimeError('Mosaico device is unavailable (code %s)' % process.returncode)
    return stdout


async def push(snapshot, device_id=None):
    config_path = Path.home() / 'Library/Application Support/MosaicoCodexPet/cloud.json'
    config = json.loads(config_path.read_text())
    payload = compact(snapshot)
    if config.get('name'):payload['source_name']=config['name']
    for task in payload['tasks']:
        task['title'] = task['title'].encode()[:96].decode('utf-8', errors='ignore')
    if payload['done']:
        payload['done']['title'] = payload['done']['title'].encode()[:96].decode('utf-8', errors='ignore')
    body = json.dumps(payload, ensure_ascii=False, separators=(',', ':')).encode()
    def upload():
        context = ssl.create_default_context(cafile=str(ROOT / 'server/server.crt'))
        request = urllib.request.Request(config['url'], data=body, method='PUT', headers={
            'Authorization': 'Bearer ' + config['write'], 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, context=context, timeout=10) as response:
            result = json.load(response)
        if result.get('ok') is not True or result.get('ts') != payload['ts']:
            raise RuntimeError('Server did not acknowledge the snapshot')
        return result
    return await asyncio.to_thread(upload)


async def run(args):
    client = AppServerClient(args.codex_bin)
    limits, threads, truncated = {}, [], False
    account_hash=""
    next_limits = next_threads = next_push = 0
    last_error = None
    last_payload = None
    retry_after = 0
    try:
        await client.start()
        while True:
            now = time.monotonic()
            try:
                if now >= next_limits:
                    account=(await client.request('account/read',{})).get('account') or {}
                    identity='|'.join(str(account.get(k,'')) for k in ('type','email','workspaceName'))
                    account_hash=hashlib.sha256(identity.encode()).hexdigest() if account.get('email') else ''
                    limits = await client.request('account/rateLimits/read')
                    next_limits = now + 60
                if now >= next_threads:
                    threads, truncated = await fetch_threads(client)
                    next_threads = now + THREAD_LIST_SECONDS
                snapshot = make_snapshot(limits, threads, truncated=truncated)
                snapshot['account_hash']=account_hash
                last_error = None
            except Exception as error:
                logging.warning('Codex data unavailable: %s', type(error).__name__)
                snapshot = make_snapshot({}, [], truncated=True)
                snapshot.update(state='offline', source_ok=False)
                last_error = error
                next_limits = next_threads = 0
                if not args.once:
                    await client.stop()
                    try:
                        await client.start()
                    except Exception:
                        await client.stop()
            save(snapshot)
            if args.once:
                print(json.dumps(compact(snapshot), ensure_ascii=False))
                if last_error:
                    raise RuntimeError('Codex data unavailable') from last_error
                return
            # Compare only device-visible content; timestamps use a quiet heartbeat.
            payload=compact(snapshot);payload.pop('ts')
            changed=payload!=last_payload
            if not args.no_device and now>=retry_after and (changed or now>=next_push):
                try:
                    await push(snapshot, args.device_id)
                    last_payload=payload
                    next_push = time.monotonic() + HEARTBEAT_SECONDS
                    retry_after=0
                except Exception as error:
                    logging.warning('Server upload failed (%s); retrying', type(error).__name__)
                    retry_after = time.monotonic() + UPLOAD_RETRY_SECONDS
            if args.duration and time.monotonic() - args.started >= args.duration:
                return
            await asyncio.sleep(SCAN_SECONDS)
    finally:
        await client.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--once', action='store_true', help='Read real data once without device writes')
    parser.add_argument('--no-device', action='store_true')
    parser.add_argument('--device-id')
    parser.add_argument('--codex-bin')
    parser.add_argument('--duration', type=float)
    args = parser.parse_args()
    args.started = time.monotonic()
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    with (CACHE.parent / 'companion.lock').open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error('A Mosaico Codex companion is already running')
        try:
            asyncio.run(run(args))
        except KeyboardInterrupt:
            pass
    return

if __name__ == '__main__':
    main()
