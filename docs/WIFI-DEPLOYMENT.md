# Wi-Fi deployment — 2026-11-02

Device: ESP-Mosaico / ESP32-S31, Device ID 4553502d4952495301001c2904d0a33a.
Application 1.1.2 preserves the approved GSP 1.5.1 scene and animation. The only
visible text change is the transport label (Wi-Fi) and the offline task message.
Normal application updates preserve Recovery 0.1.4, sysmeta, NVS and UI resources.

## Data path

The Mac collector reads Codex locally. Quota is sampled every 60 seconds; the
chat list every 2 seconds and local lifecycle events every 0.5 seconds. The
collector uploads changed device-visible JSON immediately, with a 10-second quiet
heartbeat, using a write-only credential. The board targets a 1-second poll cycle
using a separate read-only credential.
No screenshot, Codex login credential or transcript is transmitted. Task titles
and statuses are transmitted.

The relay runs on the user-provided China business server, verified via Tencent
metadata as ap-guangzhou. HTTPS endpoint: https://43.138.181.110/codex-pet/v1/state.
The existing domain's TLS connection was reset externally; the IP endpoint uses
an independent certificate with an IP SAN. Both clients verify the saved public
certificate. TLS verification is never disabled. The private key stays on server.
The certificate is valid for 1825 days from deployment; renewal requires updating
the trusted public certificate on both clients.

The service is an isolated systemd unit `codex-pet`, loopback port 18765,
source /opt/codex-pet, private SQLite /var/lib/codex-pet/pet.db. Nginx has an
independent IP virtual host; existing domain routes are preserved. Request bodies
and Authorization headers are not logged. The relay accepts only the device
schema, caps payloads at 2048 bytes, rejects out-of-order snapshots, persists the
last state and marks it offline after 90 seconds without a new upload. The device
also times out disconnected sources. In a network outage it retains UI/animations
and shows unavailable data instead of presenting stale quota as current.

Mac config locator: ~/Library/Application Support/MosaicoCodexPet/cloud.json
(mode 0600, directory 0700). Device read credential resides in its NVS namespace
pet_network. The board reuses the existing Recovery sysmeta Wi-Fi credentials
unless independently configured; credentials were not read back to the host.

## Operation

The Mac login LaunchAgent com.novatechai.mosaico-codex-pet runs the HTTPS uploader.
No USB device selector or USB data path is used by the service. The Mac must be
awake, connected to Internet and signed in to Codex for new task data.

The board needs Wi-Fi plus power, independently of the Mac's USB port. Battery
runtime and unplugged physical behavior have not been measured in this run.

To change Wi-Fi with a temporary USB connection:
`.venv/bin/python tools/provision_wifi.py` opens a local form at
http://127.0.0.1:18766. Credentials are passed through the official mosaico.py
hex-payload stdin route, which journals a payload hash rather than its contents.
The temporary form should be stopped after provisioning.

## Verified

- Real Mac upload and authenticated download with full TLS certificate checking.
- Server read/write authorization separation, validation, persistence,
  out-of-order protection and stale state behavior: 14 tests including existing
  task/allowance tests passed.
- App installed through retained Recovery, matching device identity and healthy
  firmware after restart.
- Live device logs confirmed multiple successive cloud fetches with advancing
  source timestamps. Device status reported configured=true, connected=true and
  increasing pulls. The Mac service was already changed to HTTPS, so these data
  updates did not use USB.
- Physical unplugged behavior and the live screen remain user-observed boundaries.

A newly entered Wi-Fi profile returned reason 201 (access point not found).
The firmware supports explicitly selecting the existing Recovery Wi-Fi profile
without deleting the separately saved custom profile. Configuration can be
reapplied using `tools/device_network.py --configure --use-saved-wifi`.

Final 1.1.2 observation covered 102 seconds: the board stayed connected after
reapplying saved Wi-Fi configuration and completed five HTTPS snapshot pulls
with advancing timestamps. See wifi-live-verification.json. The device boot ID
and installed ELF identity are in wifi-deployment-receipt.json.

## UI 1.2.0

All label sizes increased approximately 20 percent from the approved baseline.
The server adds `server_time` on every authenticated GET independently of the Mac
snapshot timestamp. The board displays HH:MM in Asia/Shanghai (UTC+8), advancing
between polls. Task `state` is preserved and validated end to end; green circles
mark working tasks, amber pause bars mark waiting tasks, and an outlined neutral
circle is used when state is unknown. The completion marker remains blue. Open
eyes are 76 x 96 with radius 38; timers and expression switching remain unchanged.

The real GSP native simulator passed clock presence, working/waiting icons,
blink, task drawer/back, completion and failure/offline transitions. The scene
and controller continue to be shared with device firmware. The server clock and
state-contract regression test brings the suite to 15 passing tests.

## UI 1.3.0

Task status follows the local Codex desktop sidebar metadata: an animated ring
means an active turn (including waiting within an active turn), while a green
dot means a completed result remains unread. The collector reads the desktop's
`electron-thread-read-state-v1.unreadByIdentity` without modifying it. Backend
read receipts are a fallback only when that desktop snapshot is absent; they do
not override an empty/read desktop snapshot. This desktop-private metadata may
need adaptation after future app schema changes. Existing collection scope is
local Codex chats; remote-host inventory is not added by this release.

Completed unread chats stay in the task list until read and do not inflate the
active count. The payload carries only bounded task titles, lifecycle state and
an unread boolean, never transcript bodies or local read-state identifiers.
The server validates unread booleans and preserves them. Thread inventory and
uploads target 5-second intervals (collector loop cadence may add a little
latency); device polling is 5 seconds. Quota collection remains 60 seconds.

The spinner uses a native bounded SVG rotation property, one revolution per
second, animated only while the task drawer is visible. Blink timer cadence is
167 ms (previously 250 ms): the same 19-tick cycle is now 3.173 s instead of
4.75 s, approximately 50 percent faster. Clock, source expiry and redraw timing
use elapsed milliseconds and are not sped up with the blink tick.

Home completion footer now has `刚刚完成` plus a second line containing the task
title. Long titles are shortened with an ellipsis on home; task details retain
the complete bounded title. The existing 10-minute recent-completion window and
blue completion marker remain unchanged.

Validation: 17 Python checks passed, including completed unread versus read,
active-count independence and desktop read-state precedence. The actual GSP
native backend verified rotating screenshot regions, green-dot clearing after
a read-state update, measured faster blink intervals, completion title, clock,
face tap/back and working/waiting/completed/failed/offline transitions.

## Completion notification 1.4.0

An explicit Codex task_complete event now generates a SHA-256 completion key
from thread ID, completed turn ID and completion timestamp. Only the opaque key
and timestamp are uploaded, independent of task titles or unread state. The
collector retains up to eight recent completion events in a snapshot, including
multiple completions between polls. The server validates and orders these events.

A boot-lifetime notification policy silently establishes its baseline on the
first server snapshot and remembers observed keys. Replayed snapshots and task
renames do not trigger repeated alerts; restarting does not replay history.
Events are recorded only after successfully entering the notification queue.
The hardware worker is separate from UI/network threads. It generates two 90 ms
motor pulses at 60% PWM strength, separated by 80 ms, then a softly decaying
200 ms synthesized chime at 25% codec volume. No microphone, external audio
service or downloaded media is used. Audio is muted and closed after playback.
Motor and audio errors are tracked independently so one actuator can still work
if the other fails. The device UI and its animation files are unchanged.

Diagnostic tool: `.venv/bin/python tools/notification_test.py --play` explicitly
plays one self-test; without --play it reads armed/queued/played/self-test counts
and motor/audio driver results. Self-tests are labelled separately from real
completion notifications. User-observed physical vibration/audibility remains
separate from successful driver return values.

Validation: 21 checks passed, including compiling and executing the exact device
C policy on the host, startup history suppression, replay de-duplication, concurrent
completion keys, task renaming/next turn stability and relay event validation.

### Audible chime adjustment 1.4.1

The first 1.4.0 self-test returned motor=0/audio=0, but the user confirmed only
vibration. The default codec curve maps volume 25 to -37.5 dB before hardware-gain
compensation; therefore it is not a linear 25-percent loudness setting. Version
1.4.1 uses volume 65 on that curve, a 320 ms decaying chime, 48 ms of initial
silence and 60 ms of trailing silence before mute/close so queued audio can drain.
Motor timing and notification event policy are unchanged. Physical audibility
is verified separately with a repeat self-test.

### Verified audio path and final level (1.4.2–1.4.3)

The user confirmed the original factory firmware had working sound. A 16 kHz
16-bit mono diagnostic tone was then audible on 1.4.2, but too quiet. Device logs
showed nonzero PCM peaks, PA output high, unmuted DAC register 0x31=0x00, DAC power
register 0x14=0x1a and successful register reads. The normal stereo 48 kHz short
chime had not been user-audible; the audible result does not isolate sample-rate,
channel-layout, duration and gain changes individually.

Version 1.4.3 retains 16 kHz mono, raises the codec curve setting to 90 and PCM
peak scale to 12000 (well below int16 clipping), and keeps the production chime
320 ms. --audio-only is a longer diagnostic tone; the ordinary --play self-test
uses the same short notification pattern as a genuine completion.

### User-requested final chime level and duration 1.4.4

The user heard the 1.4.3 production notification but found it quiet and short.
Version 1.4.4 lengthens the real chime to 800 ms, raises codec setting 90 -> 100
and PCM scale 12000 -> 16000, and uses a linear decay so more of the chime remains
audible. A 30 ms attack and buffered silence preserve clean startup/shutdown;
PCM remains below signed-16-bit clipping. Two 90 ms motor pulses at 60% separated
by 80 ms are unchanged. Notification de-duplication, startup baseline, UI and
Wi-Fi transport are unchanged. Codec settings are curve positions, not a linear
percentage of physical loudness.

### Approved Grok-style UI and local battery telemetry 1.5.0

The confirmed home screen uses a saturated purple (#7028E8) round head, white
angled eyes, `work ...`, a one-line recent-completion label/title and a Wi-Fi
icon at bottom right. Long home titles are truncated; task details retain the
complete received title. Server time, task spinner/unread semantics, blink cadence
and 1.4.4 notification policy/audio remain unchanged. Native C simulator checks
cover task touch/back, spinner animation, unread clearing, blink cadence, lifecycle
states and battery green/yellow/red/unknown states. Evidence: design/ui-1.5-simulator.

Battery percentage is local BQ27220 StateOfCharge (register 0x2c), refreshed every
30 seconds on a separate task. Read-only register access uses the BSP shared bus
and address; it does not call battery_init or modify gauge calibration. A voltage
read (0x08) and range checks reject invalid samples; errors display --% with an
empty gray icon. Colors: >30% green (#34C759), 11–30% yellow (#FFCC00), <=10% red
(#FF3B30). The percentage comes from the existing gauge calibration, not a new
voltage-derived estimate. RPC service 0x70 method 6 exposes only percentage, voltage
and error for device validation. No extra cloud payload or credentials are added.

The compiled bundle remains within the original 4 MiB UI partition: round heads
use native containers, small eye SVGs contain only changing expression pixels,
and task fonts share the existing 20px character atlas. Recovery, Wi-Fi settings
and device identity remain preserved by the System Update workflow.

#### Device integration correction 1.5.1

1.5.0 booted normally and read the real gauge (100%, 4197 mV), but the ninth
RPC handler exceeded the previous capacity of eight: network setup rolled back
and method 3 returned ESP_ERR_NOT_FOUND. This was detected before handoff.
1.5.1 explicitly allocates nine handlers (three Recovery + six application),
and starts the existing network service before optional battery diagnostics.
Its UI bundle and partition table are identical to 1.5.0, allowing the product's
Recovery-first application-only update. 1.5.0 is superseded, not a validated
Wi-Fi release. Final live validation is stored separately as 1.5.1 evidence.

### Faster status delivery and long-turn counting 1.5.2

Local activity scans run every 0.5 seconds; thread inventory refreshes every two
seconds. A changed device-visible snapshot uploads on the same loop instead of
waiting for a periodic batch; unchanged data gets a 10-second freshness heartbeat.
Failed uploads retry after two seconds. Quota lookup remains every 60 seconds.

Lifecycle reading now indexes each file once and resumes from a validated cursor,
rather than looking at only the final 2 MiB. This fixes active long turns whose
start boundary fell outside the previous tail window. Initial live indexing took
1158 ms across 15 chats; repeated scanning took 7 ms and correctly identified
three running chats, including the two shown in the user's screenshot. Cursors
handle replacement/truncation and incomplete final lines; they store lifecycle
metadata, not message contents. 23 tests cover these behaviors and existing
notification/read-state contracts.

Device fetch cycles target 1000 ms including the preceding request duration.
A slow request skips catch-up bursts; existing connection/recovery/TLS checks
remain unchanged. Layout, expressions, battery sampling and notification behavior
are unchanged from 1.5.1. Typical status visibility is expected in roughly 1–3
seconds on a healthy connection; this is a target, not a measured physical-screen
latency guarantee. Final polling observations are stored in ui-1.5.2-validation.json.

### Half-second fetch and slightly quieter chime 1.5.3

Device fetch cycles target 500 ms, including request duration, with no catch-up
burst after a slow request. Mac change detection remains 0.5 seconds, with
immediate changed-state upload and a 10-second quiet heartbeat. This is a fetch
interval, not a guarantee of 0.5-second end-to-end display latency.

The same 800 ms mono chime now uses PCM peak scale 12500 instead of 16000
(approximately -2.14 dB). Codec curve setting 100, envelope, duration, frequencies,
notification de-duplication and both vibration pulses remain unchanged. A single
production-pattern self-test checks the hardware path; subjective loudness is
user-observed. Final validation is stored in ui-1.5.3-validation.json.

### Further chime reduction 1.5.4

At the user's request, PCM peak scale is reduced again from 12500 to 9500
(approximately -2.38 dB relative to 1.5.3). Chime length remains 800 ms, codec
curve setting remains 100, and vibration, UI, notification policy and 500 ms
network fetch cadence remain unchanged. Subjective volume remains user-observed.

### User-selected three-note completion chime 1.5.5

The user selected option 3 after four Mac auditions. The production notification
now embeds the exact 800 ms tone samples from design/chime-options/03-sparkle.wav
(783.99, 987.77 and 1174.66 Hz), retaining the 9500 PCM peak and existing codec
settings. tools/generate_chime.py packages the approved waveform as chime.pcm;
the component embeds it in the application image. Opening/drain silence, two
vibration pulses, completion de-duplication, 500 ms fetching and all UI behavior
are unchanged. Audio-only diagnostic mode retains its existing single test tone.

### Buffered notification audio 1.5.6

The user reported a break in the middle of the three-note sound, rather than a
UI stall. The previous priority-2 worker supplied 128-frame writes while priority-3
TLS fetching ran twice per second; starvation is a plausible cause, not proven
by the earlier zero-error return codes. Production audio now uses a lifetime-owned
25.6 KiB PSRAM buffer with the exact approved waveform and a single codec write.
The worker priority is 5 so DMA feeding is not delayed by the network worker;
codec writes block on DMA and do not busy-wait for the 800 ms duration. Diagnostic
mode, volume, note timings, vibration, UI and fetch interval are preserved.

RPC notification status adds stream_ms, the wall time spent supplying tone data.
This is supporting timing evidence, not proof of the absence of audible gaps.
Physical continuity requires the user's listening confirmation.

### Settings, geometry-only UI, connection reuse and wireless updates 2.0

The home layout retains the approved purple character. A visible gear opens a
settings page for volume, sound preview, vibration and brightness (10–100%).
Values are persisted in pet_settings NVS after a 750 ms debounce, with work on
a separate queue so flash writes and brightness I2C do not block UI callbacks.
Volume 35 is the previous 9500-peak level; zero mutes sound independently of
vibration. All face artwork, Wi-Fi and animated task rings use native geometry:
there are zero image objects and no SVG/raster image references in the scene.
Font atlases and the approved PCM audio remain necessary resources.

The HTTPS client stays open after complete responses and reconnects on errors
or settings changes. RPC status exposes actual connection count beside pulls.
The relay supports HTTP/1.1 and carries validated done_count; Mac counting groups
completion events within the latest one-second timestamp window. Device alerts
coalesce queued completion events for one second, without replaying startup
history, and play one alert per burst. Manual previews remain immediate.

Wireless firmware updates use the existing paired ESP-Iris TCP service and
retained Recovery workflow, not an application-owned flash writer. One initial
USB-only pairing operation stores a random token in the existing sysmeta NVS;
the Mac copy is kept outside the project in the private application-support
folder. mDNS resolves the normal device after address changes. The Mac updater
selects an .irisfw bundle and invokes mosaico.py over the paired LAN endpoint.
Recovery, device identity, Wi-Fi configuration and settings stay preserved.
A retained Recovery entry remains available if an update is interrupted; this
single-application layout does not claim automatic fallback to a previous app.

#### Installed acceptance: 2.0.1

The first 2.0.0 installation used USB. A subsequent full 2.0.1 bundle update
completed through the paired TCP endpoint, including retained Recovery and normal
reconnection. Final live identity reports app 2.0.1, TCP transport and a new Boot
ID on the original device. Settings changed for testing survived that update and
were restored to volume 35, brightness 100 and vibration on. A three-event burst
produced one playback, last_batch=3 and merged=2, with audio/motor error=0 and
795 ms feeding time. During playback pulls advanced 100 -> 107 while the HTTPS
connection count remained 1. Bonjour hostname resolution and rejection of invalid
settings without applying partial values were also verified.

The Mac launcher is tools/桌宠无线更新.app; choose an .irisfw bundle while the Mac
and device are on the same LAN. The private pairing files stay outside the project.
Evidence: ui-2.0.1-validation.json, wireless-2.0.1-update.log and native settings
interaction captures under design/ui-2.0-simulator. 25 automated tests passed.

### Quiet 50% default, independent sound switch and six colors 2.1

The previous level mapping used the historical 35 setting as unity gain, so
50 increased the source amplitude rather than providing a gentle midpoint.
Version 2.1 replaces this with the shared audio_level scaler: quadratic level
with a half-source ceiling. The approved three-note source peaks at 9500;
50 now produces peak 1187, 25 produces 296 and 100 produces 4750. These are PCM
amplitudes, not claims of an exact physical loudness percentage. The codec's
known-working hardware configuration and continuous buffered playback remain.

A separate persisted sound flag prevents all speaker writes for muted alerts
and previews while vibration stays independent. Zero level also remains silent.
Runtime notification status reports applied_volume, pcm_peak, audible_plays and
muted_events so a settings display alone is not used as proof of attenuation.
One-time settings schema 2 migration initializes level 50, retains an existing
zero-level mute, and preserves the user's brightness/vibration. Subsequent boots
preserve level, sound and chosen color, with persisted values readable through RPC.

Settings and Wi-Fi are at the upper left; CODEX occupies the former gear area
beside the work status. The settings page has six code-drawn color swatches:
purple, blue, mint green, orange, pink and cyan. Dynamic native colors update all
six expression states. Sound-off disables the preview button; idle settings no
longer enqueue unchanged labels and swatch states on every timer tick. No image
references are introduced. Final physical and native validation is retained under
ui-2.1-simulator and ui-2.1.0-validation.json.

#### Installed acceptance: 2.1.0

A paired Wi-Fi System Update completed and the original device returned in normal
mode with a new Boot ID and version 2.1.0. Migration readback was volume 50,
sound on, purple selected, with the user's existing brightness 80 and vibration
on preserved. Live tests verified no speaker playback while sound was off,
peak 296 at 25, and peak 1187 at 50; buffered feed time remained below 950 ms.
Every color index 0–5 was stored and independently read from NVS. Final settings
were restored to 50, sound on, purple, brightness 80 and vibration on. Native
pointer tests covered dragging volume, sound-off preview disabling, six visible
home colors, new header/brand positions, task drawer, blink and status states.
26 automated tests passed. Evidence: ui-2.1.0-validation.json and
wireless-2.1.0-update.log. Physical loudness remains a user listening judgment.
