"""Read-only adapter for desktop unread state; never modifies Codex."""
import json
import sqlite3
from pathlib import Path


def unread_thread_ids(codex_home=None):
    root = Path(codex_home) if codex_home else Path.home()/'.codex'
    # The running desktop owns its sidebar read state. Its empty set is meaningful;
    # do not resurrect historical backend receipts after the user has read a task.
    try:
        state=json.loads((root/'.codex-global-state.json').read_text())
        identities=state['electron-thread-read-state-v1']['unreadByIdentity']
        if isinstance(identities,dict):
            unread=set()
            for hosts in identities.values():
                if not isinstance(hosts,dict):continue
                for host,ids in hosts.items():
                    if host.startswith('local:') and isinstance(ids,list):
                        unread.update(thread_id for thread_id in ids if isinstance(thread_id,str))
            return unread
    except (OSError,ValueError,KeyError,TypeError):
        pass
    # Fallback for installations that only persist backend read receipts.
    try:
        with sqlite3.connect('file:'+str(root/'state_5.sqlite')+'?mode=ro',uri=True,timeout=1) as db:
            return {thread_id for thread_id,turn in
                    db.execute('SELECT thread_id, first_unread_turn FROM thread_read_receipts') if turn is not None}
    except sqlite3.Error:
        return set()
