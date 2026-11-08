# Snapshot v1

Transport: official `mosaico.py iris rpc 0x70 1 --payload <JSON>` under the verified
project/device ownership. Max request 2048 UTF-8 bytes. Device replies `OK` after
placing a validated copy into its latest-value mailbox; a reply alone does not
prove rendered output. Normal application only; Vibe Mode owns OTA writing.

Fields: `v=1`, `ts` Unix seconds, `state` one of offline/idle/working/waiting/
completed/failed, `active` 0..9999, `partial` bool, `ok` bool. `windows` has zero,
one or two objects: `label`, `remaining` 0..100, `reset_at` Unix seconds, `minutes`.
The current compact home screen shows the primary window. `tasks` holds up to four
`{title,state}` objects. `done` is null or `{title,at}` with an explicit successful
completion timestamp. UI blue marker expires at `at+600` regardless of repeated
heartbeats; disconnect hides it. Task drawer shows the recent completed title.

The device receives no OAuth tokens, user prompts, assistant replies, transcript
paths or thread IDs. It trusts the Mac to supply account data; malformed requests
are rejected. The Mac collector only reads Codex/local rollout files and writes
its own private cache. It does not modify Codex login/config or install global hooks.
