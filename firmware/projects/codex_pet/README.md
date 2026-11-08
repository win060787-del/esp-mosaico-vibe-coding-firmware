# Codex Pet firmware

ESP-Mosaico ESP32-S31; GSP 1.5.1. Black background and white animated face.
Shows actual Codex primary allowance, reset days/hours, active task count and
blue recent-completion marker. Tap the face for up to four task titles and the
last completion. Visible back button returns to the face.

`main` composes device services. `app_pet` is the portable controller shared with
the native simulator. `app_link` validates Iris RPC snapshots and passes them
through a one-slot mailbox. `app_display` retains official board policy.

Run builds and installations from the parent ESP-Mosaico workspace through its
pinned toolchain and `mosaico.py`. See the enclosing project's README for the Mac
companion and validation status. Never use generic full-flash erase commands.

Generate the authored GSP scene with `python tools/generate_scene.py`. The font
subset is built by `tools/build_font.py` from official Noto Sans SC. Generated
GSP bindings/bundles stay in build output. Application resources reserve 4 MiB
because runtime Chinese task titles require a full GB2312 glyph corpus. Vibe Mode, sysmeta, NVS and Core Dump partition definitions retain the official
layout. The application update bundle writes only the partition table, main
application and UI resources; it does not include a Vibe Mode image.
