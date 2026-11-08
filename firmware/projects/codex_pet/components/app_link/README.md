# Mac link

Application-only Iris RPC service 0x70 method 1 accepts bounded UTF-8 JSON v1 snapshots
from the Mac companion. Rejects unknown states, malformed numbers, oversized payloads
and invalid field types. Owns a one-element FreeRTOS mailbox; newer snapshots replace
older ones. RPC never mutates GSP; the UI thread consumes copied values nonblocking.
Created once at boot; lives until reboot. Depends on app_pet, ESP-Iris, cJSON and FreeRTOS.
No Codex credentials, prompts or assistant replies cross USB.
