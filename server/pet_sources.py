"""Multi-computer storage, selection and one-time enrollment for one pet owner."""
import copy,hashlib,json,re,secrets,sqlite3,time

FRESH_SECONDS=90
MAX_TERMINALS=64
MODES={'auto','fixed','all'}

def clip(text,limit=96):
    return text.encode('utf-8')[:limit].decode('utf-8',errors='ignore')

def source_id(value):
    return isinstance(value,str) and (value=='legacy' or bool(re.fullmatch('[a-f0-9]{32}',value)))

def source_name(value):
    return isinstance(value,str) and 0<len(value.encode())<=96 and all(ord(c)>=32 for c in value)

class MultiStore:
    def __init__(self,path):
        self.path=path
        with sqlite3.connect(path) as db:
            db.execute('CREATE TABLE IF NOT EXISTS snapshot (id INTEGER PRIMARY KEY CHECK(id=1),ts INTEGER NOT NULL,body TEXT NOT NULL)')
            db.execute('CREATE TABLE IF NOT EXISTS terminals (id TEXT PRIMARY KEY,name TEXT NOT NULL,writer_hash TEXT UNIQUE,created INTEGER NOT NULL)')
            db.execute('CREATE TABLE IF NOT EXISTS terminal_state (id TEXT PRIMARY KEY,ts INTEGER NOT NULL,body TEXT NOT NULL)')
            db.execute('CREATE TABLE IF NOT EXISTS viewers (id TEXT PRIMARY KEY,current TEXT NOT NULL)')
            db.execute('CREATE TABLE IF NOT EXISTS enroll_codes (digest TEXT PRIMARY KEY,expires INTEGER NOT NULL)')
            db.execute("INSERT OR IGNORE INTO terminals VALUES('legacy','Mac',NULL,?)",(int(time.time()),))
            old=db.execute('SELECT ts,body FROM snapshot WHERE id=1').fetchone()
            if old:db.execute("INSERT OR IGNORE INTO terminal_state VALUES('legacy',?,?)",old)

    def put(self,data,terminal='legacy'):
        with sqlite3.connect(self.path) as db:
            if not db.execute('SELECT 1 FROM terminals WHERE id=?',(terminal,)).fetchone():raise ValueError('unknown terminal')
            body=json.dumps(data,ensure_ascii=False,separators=(',',':'))
            db.execute('INSERT INTO terminal_state VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET ts=excluded.ts,body=excluded.body WHERE excluded.ts>=terminal_state.ts',(terminal,data['ts'],body))
            if terminal=='legacy':db.execute('INSERT INTO snapshot VALUES(1,?,?) ON CONFLICT(id) DO UPDATE SET ts=excluded.ts,body=excluded.body WHERE excluded.ts>=snapshot.ts',(data['ts'],body))
            if source_name(data.get('source_name')):db.execute('UPDATE terminals SET name=? WHERE id=?',(data['source_name'],terminal))

    def writer(self,token):
        digest=hashlib.sha256(token.encode()).hexdigest()
        with sqlite3.connect(self.path) as db:
            row=db.execute('SELECT id FROM terminals WHERE writer_hash=?',(digest,)).fetchone()
        return row[0] if row else None

    def register(self,identity,name,db=None):
        if not source_id(identity) or identity=='legacy' or not source_name(name):raise ValueError('invalid terminal')
        if db is None:
            with sqlite3.connect(self.path) as connection:return self.register(identity,name,connection)
        if db.execute("SELECT COUNT(*) FROM terminals WHERE id!='legacy'").fetchone()[0]>=MAX_TERMINALS:raise ValueError('terminal limit')
        token=secrets.token_urlsafe(32)
        db.execute('INSERT INTO terminals VALUES(?,?,?,?)',(identity,name,hashlib.sha256(token.encode()).hexdigest(),int(time.time())))
        return {'source_id':identity,'name':name,'write':token}

    def enrollment_code(self):
        code=secrets.token_hex(5).upper();expires=int(time.time())+600
        with sqlite3.connect(self.path) as db:
            db.execute('DELETE FROM enroll_codes')
            db.execute('INSERT INTO enroll_codes VALUES(?,?)',(hashlib.sha256(code.encode()).hexdigest(),expires))
        return {'code':code[:5]+'-'+code[5:],'expires':expires}

    def enroll(self,code,identity,name):
        if not isinstance(code,str):raise ValueError('invalid code')
        normalized=code.replace('-','').strip().upper()
        if not re.fullmatch('[0-9A-F]{10}',normalized):raise ValueError('invalid code')
        with sqlite3.connect(self.path) as db:
            db.execute('BEGIN IMMEDIATE')
            digest=hashlib.sha256(normalized.encode()).hexdigest()
            row=db.execute('SELECT expires FROM enroll_codes WHERE digest=?',(digest,)).fetchone()
            if not row or row[0]<time.time():raise ValueError('expired code')
            result=self.register(identity,name,db)
            db.execute('DELETE FROM enroll_codes WHERE digest=?',(digest,))
            return result

    def _rows(self):
        with sqlite3.connect(self.path) as db:
            rows=db.execute('SELECT t.id,t.name,s.body FROM terminals t LEFT JOIN terminal_state s ON t.id=s.id ORDER BY t.created,t.id').fetchall()
        # Hide the historical single-publisher entry once named computers exist.
        if any(i!='legacy' for i,_,_ in rows):rows=[r for r in rows if r[0]!='legacy']
        return [{'id':i,'name':name,'data':json.loads(body) if body else None} for i,name,body in rows]

    @staticmethod
    def online(row,now):
        return bool(row['data'] and row['data'].get('ok') and now-row['data']['ts']<=FRESH_SECONDS)

    def _select(self,rows,mode,fixed,viewer,now):
        if mode not in MODES:raise ValueError('invalid mode')
        if not source_id(viewer) and not re.fullmatch('[a-f0-9]{32}',viewer):raise ValueError('invalid viewer')
        with sqlite3.connect(self.path) as db:
            old=db.execute('SELECT current FROM viewers WHERE id=?',(viewer,)).fetchone()
            current=next((r for r in rows if old and r['id']==old[0]),None)
            if mode=='fixed':return next((r for r in rows if r['id']==fixed),None)
            live=[r for r in rows if self.online(r,now)]
            running=[r for r in live if r['data']['active']>0]
            pool=running or live
            chosen=current if current in pool else next(iter(pool),current or next(iter(rows),None))
            if chosen:db.execute('INSERT INTO viewers VALUES(?,?) ON CONFLICT(id) DO UPDATE SET current=excluded.current',(viewer,chosen['id']))
            return chosen

    def catalog(self,page=0,mode='auto',fixed='',viewer='legacy'):
        rows=self._rows();now=int(time.time());selected=self._select(rows,mode,fixed,viewer,now)
        items=[{'id':r['id'],'name':clip(r['name'],48),'online':self.online(r,now),'active':r['data']['active'] if self.online(r,now) else 0} for r in rows]
        return {'terminals':items[page*4:page*4+4],'page':page,'total':len(items),'selected':selected['id'] if selected else ''}

    def get(self,mode='auto',fixed='',viewer='legacy'):
        rows=self._rows();now=int(time.time());chosen=self._select(rows,mode,fixed,viewer,now)
        if not chosen:return None
        live=[r for r in rows if self.online(r,now)]
        if mode=='all':data=self._aggregate(live,chosen,now)
        else:
            if not chosen['data']:return None
            data=copy.deepcopy(chosen['data'])
            if not self.online(chosen,now):data.update(ok=False,state='offline',ts=now)
            data['source']={'id':chosen['id'],'name':clip(chosen['name'],48),'mode':mode}
        if mode in ('auto','all'):self._completed(data,rows,now,True)
        else:self._completed(data,[chosen],now,False)
        candidates=[r for r in (rows if mode in ('auto','all') else [chosen]) if r['data'] and r['data'].get('recent')]
        if candidates:
            row=max(candidates,key=lambda r:r['data']['recent']['at'])
            data['recent']=copy.deepcopy(row['data']['recent'])
            if len(rows)>1 and mode in ('auto','all'):data['recent']['title']=clip(row['name']+' · '+data['recent']['title'])
        data['server_time']=now
        # Keys and auxiliary metadata aren't needed in the fixed device snapshot.
        for task in data.get('tasks',[]):task.pop('key',None)
        for key in ('account_hash','source_name'):data.pop(key,None)
        # Keep the existing firmware's bounded 2 KiB contract during migration.
        while len(json.dumps(data,ensure_ascii=False,separators=(',',':')).encode())>2000 and len(data['completions'])>1:data['completions'].pop(0)
        if len(json.dumps(data,ensure_ascii=False,separators=(',',':')).encode())>2000:
            for task in data['tasks']:task['title']=clip(task['title'],64)
        while len(json.dumps(data,ensure_ascii=False,separators=(',',':')).encode())>2000 and data['tasks']:
            data['tasks'].pop();data['partial']=True
        return data

    @staticmethod
    def _completed(data,rows,now,prefix):
        events={};latest=None
        for row in rows:
            d=row['data'] or {}
            for event in d.get('completions',[]):
                if 0<=now-event['at']<600:events[event['key']]=event
            done=d.get('done')
            if done and 0<=now-done['at']<600 and (not latest or done['at']>latest['at']):
                latest={'at':done['at'],'title':clip((row['name']+' · ' if prefix and len(rows)>1 else '')+done['title'])}
        data['completions']=sorted(events.values(),key=lambda x:x['at'])[-8:]
        data['done']=latest
        data['done_count']=sum(e['at']>=latest['at']-1 for e in data['completions']) if latest else 0

    @staticmethod
    def _aggregate(rows,chosen,now):
        base=copy.deepcopy((chosen or {}).get('data') or {})
        if not base:return None
        tasks={};unknown=0
        for row in rows:
            d=row['data'];visible=0
            for task in d['tasks']:
                running=task.get('state') in ('working','waiting')
                if running:visible+=1
                key=(d.get('account_hash') or row['id'],task.get('key') or row['id']+task['title'])
                copy_task={**task,'title':clip(row['name']+' · '+task['title'])}
                old=tasks.get(key)
                if old is None or (running and old.get('state') not in ('working','waiting')):tasks[key]=copy_task
            unknown+=max(0,d['active']-visible)
        running=[t for t in tasks.values() if t.get('state') in ('working','waiting')]
        other=[t for t in tasks.values() if t not in running]
        base.update(ts=max((r['data']['ts'] for r in rows),default=now),active=len(running)+unknown,partial=bool(unknown) or any(r['data']['partial'] for r in rows),tasks=(running+other)[:4],ok=bool(rows),state='waiting' if any(t.get('state')=='waiting' for t in running) else 'working' if running or unknown else 'idle')
        accounts={r['data'].get('account_hash') or r['id'] for r in rows}
        if len(accounts)>1:base.update(windows=[],quota_label='按终端查看')
        base['source']={'id':'all','name':'全部终端','mode':'all'}
        return base
