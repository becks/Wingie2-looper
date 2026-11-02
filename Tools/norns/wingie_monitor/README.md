# wingie monitor (norns)

The [Loop Monitor](../../loop_monitor.html) on [norns](https://monome.org/docs/norns/):
both loopers on the norns screen, with transport (record, play, overdub, stop), speed,
loop level and all the Loop Monitor controls on the keys and encoders. It talks to the
Wingie2 over its USB serial port, so a single USB cable does everything; no MIDI interface
is needed.

## Requirements

- **norns** with the `serial` module (release 250926 or later). To check, type `serial` in
  the maiden REPL: it should print `table: 0x…`, not `nil`.
- **Wingie2 firmware v4.30-duallooper.11 or later** (the release with `looper_state` and
  `looper_level`). Older firmware still shows everything, but K2/K3 on the LOOP page and the
  `level` row answer "flash firmware with looper_state". The `start` and `end` rows need
  v4.30-duallooper.12 or later.
- A **USB data cable** from the Wingie2 to a norns USB port. A charge-only cable powers the
  Wingie2 but norns never sees it (the screen stays on "waiting for Wingie2…").
- A **norns power supply that holds 5.1 V at 3 A** (the official Raspberry Pi 4 supply or
  equivalent). norns also powers the Wingie2 through USB; with a weak supply, plugging or
  unplugging the Wingie2 makes norns reboot, and a reboot while a file is being written can
  corrupt it. `vcgencmd get_throttled` (over SSH) should print `throttled=0x0`. A powered USB
  hub between norns and the Wingie2 also avoids the problem.

## Install

**From maiden** (norns.local in a browser): paste this line in the REPL at the bottom of the
page and press Enter. It downloads this repository and copies the script to
`~/dust/code/wingie_monitor`; run it again to update.

```lua
os.execute("rm -rf /tmp/wl && git clone -q --depth 1 https://github.com/becks/Wingie2-looper /tmp/wl && rm -rf /home/we/dust/code/wingie_monitor && cp -r /tmp/wl/Tools/norns/wingie_monitor /home/we/dust/code/ && rm -rf /tmp/wl && sync")
```

**From a computer** with a copy of this repository (password `sleep` unless you changed it):

```bash
scp -r Tools/norns/wingie_monitor we@norns.local:~/dust/code/
```

Then, on norns: K1 → SELECT → `wingie_monitor` → K3. Connect the Wingie2 before or after
loading; the script finds it either way. The first time, the screen shows "waiting for
Wingie2…" until the Wingie2 answers, then both loops.

Opening the port from norns does not reset the Wingie2 (unlike the browser Loop Monitor),
so loops recorded before connecting are kept. The browser Loop Monitor, the configuration
page and the web flasher cannot use the Wingie2 while it is connected to norns.

## As a mod

The same folder is also a norns **mod**: the same pages in the system menu, next to any
other script, so you can run a script on norns and control the Wingie2 at the same time.

1. SYSTEM → MODS → `WINGIE_MONITOR`, E3 to enable it (a `+` appears), then SYSTEM → RESTART.
2. To open the pages: K1 for the system menu, then with E2 and K3 go to SYSTEM → MODS →
   `WINGIE_MONITOR` (now with `>`) and press K3.

To leave, press K1: you are back in the running script, and K1 again returns straight to the
pages. Or turn E1 all the way left, past LOOP A, for the list of mods. Loops keep playing
either way, since they run on the Wingie2.

To disable the mod: SYSTEM → MODS → `WINGIE_MONITOR`, E3 (the `+` goes away), then
SYSTEM → RESTART.

| Control | In the mod |
|---|---|
| K1 | to the running script; K1 again comes back to these pages |
| E1 | page, as in the script; to the left of LOOP A, back to the list of mods |
| K2, K3, E2, E3 | as in the script |

The mod polls the Wingie2 only while its pages are showing, so it costs the running script
nothing the rest of the time. Loading `wingie_monitor` as a script still works, with or
without the mod. With the mod enabled, its code is loaded at startup: after updating the
files, restart norns (SYSTEM → RESTART).

While enabled, the mod takes every CH340 or CP210x serial device at startup, so another
script cannot use one of these on the same norns; disable the mod for that.

## Pages

E1 changes page.

| Page | Shows | Controls |
|---|---|---|
| LOOP A, LOOP B | both loops (B on top, A below, as on the panel), the page's loop highlighted: state, length, speed, waveform and play head, playback window (brackets; outside it dim), loop level | for the page's loop: K2 record → (again) play → overdub ↔ play; K3 stop ↔ play (a recording closes and plays); E2 start and E3 end of the playback window (seconds shown top right while moving; 1/1000 of the recording per step, more when turned fast) |
| CTRL | the Loop Monitor controls in two columns, A and B, speed included | K2/K3 row up/down, E2 value A (left), E3 value B (right); shared rows take either |
| NOTES | the note loops: state, events, length, events on 12 rows, cursor | |
| PANEL | mode, octave, last note and held keys per side; mix, volume, decay; thresholds and clip gains with their keys; line/mic and mono | |
| METERS | input and output levels, scrolling (about 1 s), dB and clip | |
| SYS | firmware, round trip, polls, timeouts, parse errors, reboots seen | K3 reload waveforms, clear max RTT |

K2 on a stopped loop starts a **new** recording (the old loop is replaced), as MIDI CC 80 = 1.

CTRL rows: speed (step), speed mode, SOS, loop level, mode, pan, gate, start and end of the
playback window (in seconds into the recording; 1/1000 of it per step, more when turned fast;
the edges never cross), the side filter (type off/LP/HP/BP, cutoff in Hz, a semitone per
step, and resonance; needs v4.30-duallooper.13), position (pre/post the resonator), and the shared live input, looper
latch and line input mono/stereo. Line input
mono/stereo is applied at once but kept over power-off only after a Save (configuration page,
or both Mode buttons held for 3 s); the other rows behave like their MIDI CCs (runtime only,
except the latch and live input, saved like the other settings). A value just sent is shown
straight away and the polled state takes over after 0.6 s.

Reverse and cross-feedback have no serial command: use the panel or MIDI CC 85 and 84.

## Troubleshooting

- **"waiting for Wingie2…"**: check the cable (data, not charge-only). Over SSH, `lsusb`
  should list `1a86:7523 QinHeng Electronics CH340`. Maiden prints every serial device the
  script sees (`wingie monitor: serial device …`); if the Wingie2 is not accepted, add its
  `vendor`/`model` to `is_bridge` in `lib/core.lua`.
- **"no answer: timeout"**: the port opened but the Wingie2 did not answer; reload the script.
  The SYS page shows round trips and timeouts.
- **norns stuck after SYSTEM → RESTART, when you have used SSH since it booted**: seen once
  with a restart sent from the REPL, with `JACK semaphore error` in the log. Turn norns off and
  on again, or over SSH run `sudo systemctl stop "norns-*"; sudo systemctl start norns.target`.
- **"load fail" after a reboot**: the files may have been corrupted by a reboot during a
  write (see the power supply above); install again.
- **Digital noise when the Wingie2's audio out goes into the norns audio in**: a ground
  loop. The Wingie2 has no battery and takes its power and ground over USB (from norns or a
  computer), so the audio cable closes a second ground path and USB/digital noise reaches
  the norns input. It is not the script: the noise is there with the Wingie2 on a computer
  too, and it disappears when the Wingie2 runs from a USB power bank. To keep the serial
  connection and clean audio, put a **USB isolator** (ADuM3160 type, 12 Mbps full speed;
  pick one with an external power input, since many cannot supply much current on the
  isolated side) between norns and the Wingie2, or a **ground loop isolator** (audio
  transformer) on the audio cable. To play without the script, a power bank alone is enough.

## Notes for developers

The code is in `lib/core.lua`; `wingie_monitor.lua` (script) and `lib/mod.lua` (mod) are thin
front ends. The core avoids `clock.run` and recreates its metros, because loading any script
cancels all clocks and frees all metros (`Script.clear`).

Two norns (250926) `serial` quirks are handled in the script: devices are only offered to
handlers when udev reports them, so a Wingie2 already plugged in is re-announced with
`sudo udevadm trigger` at load; and matron applies `cflag` after the speed, so the baud
rate is also written into the CBAUD bits of `cflag` (otherwise the port stays at 9600).
On reload the script takes over the already open port. The protocol is the one of the
browser Loop Monitor (`@{json}` requests, `<{json}` answers, 115200 8N1); see
[`DUAL_LOOPER.md`](../../../DUAL_LOOPER.md).
