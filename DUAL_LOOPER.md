# Wingie 2 — Two Asynchronous Loopers

Each mono input feeds an independent looper followed by the original resonator for
that channel; each side's output (resonator plus dry signal) then goes through a stereo
pan (see the README; by default left stays hard left and right hard right). A side's looper
can instead sit after the resonator and pan (CC 88), recording and playing that output
channel while the resonator keeps sounding the live input at full level. The resonator
modes, notes, and controls remain unchanged. The looper is an additional processing
layer, not a new selector mode. It can be controlled over
TRS MIDI using the configured left and right channels (normally 1 and 2); the Both
channel controls both loopers. Disable MPE in the USB configuration to access these
CC messages.

Essential controls are also available from the front panel. Hold both Mode buttons
and use the note keyboard on the desired side: C starts a new recording, C# switches
to playback, D enables overdub, D# returns to bypass while preserving the loop, and
holding E for one second clears the loop. A looper gesture suppresses the normal mode
change and save actions associated with the Mode buttons. Holding both Mode buttons
for three seconds without pressing a note continues to save preferences normally.
With the looper latch on (toggled by holding both B keys at power-on, red for on and white
for off, or by Looper Latch in the configuration page; saved), a tap of both Mode buttons
locks the looper layer instead of changing both sides' modes, and another tap unlocks it.
While locked, the keys give the commands below without holding Mode, including the E, F, F#,
G and A holds, and both LEDs flash red for 120 ms every 1.2 s (white in Bar mode). Holding a
single Mode button still sets threshold or gain, and its tap still changes that side's mode.
While holding both Mode buttons, F and G move that side's loop speed one step down or
up through pure-ratio fourths, fifths and octaves (1/4, 1/2, 2/3, 3/4, 1, 4/3, 3/2, 2);
holding F or G for half a second jumps an octave from the current speed instead (within
0.25–2×), and F# returns to 1×. Holding F# for half a second cycles the side's speed
mode: varispeed (tape: pitch and duration together), pitch (the speed shifts pitch, the
loop keeps its length) and stretch (the speed changes tempo, the pitch is kept); the LED
blinks seven times, white, purple or yellow. G# lowers and A#/B♭ raises that side's
loop level in approximately 6% steps. B toggles reverse playback. Holding C# for half a
second toggles gate mode (the press still starts playback; six blinks, red on, white off);
holding G# for half a second moves the loop before or after the resonator (the level step of
the press is undone; six blinks, yellow before, purple after). See the README for both.

### Note loopers

Each side also has a panel key looper. While holding both Mode buttons, A starts
recording that side's note keys; release the Mode buttons and play normally. Hold both
Mode buttons and press A again to close the loop: its length is the time between the two
A presses, and playback starts immediately. During playback, A stops the sequence and
the next A restarts it from the beginning. Holding A for one second clears it; a new A
then records again. Up to 256 key presses per side are stored, and a full buffer closes
the loop by itself.

Recorded keys are replayed at the current octave, so the octave selector transposes
a running sequence. They follow the side's current mode: Poly and Ratio cycle voices,
String, Bar and the other pitched resonator modes set the note without changing the tap sequence, and Cave mode neither
records nor replays keys (its keys edit resonator mutes). Only panel keys are recorded,
not incoming MIDI notes, and sequences are lost at power-off. The note and audio loopers
run independently and are not synchronized.

| CC | Function | Value |
|---|---|---|
| 80 | State | 0 bypass/stop; 1 new recording; 2 playback; 3 overdub; 4 clear |
| 81 | Speed/pitch | 0 approximately half speed; 64 normal; 127 approximately double speed |
| 82 | Loop level | 0–127 |
| 83 | Material retained during overdub (sound on sound) | 0–127; default approximately 80% |
| 84 | Cross-feedback during overdub | 0–127 = 0–25% of the opposite output |
| 85 | Reverse | 0–63 forward; 64–127 reverse |
| 86 | Speed step | 0–127 in eight bands of 16: 1/4, 1/2, 2/3, 3/4, 1 (64), 4/3, 3/2, 2 |
| 87 | Speed mode | 0–42 varispeed; 43–85 pitch (length kept); 86–127 stretch (pitch kept) |
| 88 | Loop position | 0–63 before the resonator; 64–127 after it |
| 89 | Gate mode | 0–63 off; 64–127 on (held keys and notes play the loop from its start) |

SOS (sound on sound, CC 83) is the share of the existing loop kept on each overdub pass:
at 0% each pass replaces the loop, at 100% nothing fades. It is set per side, is not
saved, and returns to 80% at power-on. Besides CC 83 it can be set from the Loop Monitor
(below) or over USB serial with `set_param` `sos` (0–1 in 0.01 steps, clamped).

Send CC80=1 on channel 1, then CC80=2 to close loop A. Repeat on channel 2 with a
different duration for loop B. CC80=3 adds the live input and feedback to the existing
loop. CC80=0 preserves the loop but returns to the live input. Recordings are volatile
and disappear when the instrument is powered off.

Audio is stored as 8-bit G.711 μ-law at 4.9 kHz while the codec and DSP continue to run
at 44.1 kHz. Companding gives quiet and medium-level material more resolution than
linear 8-bit PCM without increasing memory use. Each stored sample is the average of
nine input samples, and playback is linearly interpolated. Loop memory is allocated as
2 KB chunk pairs (one chunk per side) spread across the internal-RAM heap regions, so
small heap fragments are used too. While the chunks are taken, one 12 KB block is held
and then freed, so the audio and control task stacks created afterwards always find
contiguous room; another 8 KB stays free for runtime use (about 9 KB remains free while
running). The firmware
links neither WiFi nor Bluetooth (not even `btStop()`), so the core releases the
Bluetooth controller reserve and the WiFi statics are not included. The measured
180,224-byte allocation (44 chunks per side) provides approximately 18.39 seconds per
side (17.55 s up to v4.30-duallooper.6); the boot log prints the actual figure on a
`LOOPER_MEM allocated=` line. PSRAM is not
available on the tested unit. Recording automatically switches to playback when the
buffer is full; that side's LED then blinks red/white for about 0.75 s. During playback
and overdub, the LED flashes purple (white in Cave mode) for about 120 ms each time the
loop restarts. If allocation fails, the signal remains in bypass. Cross-feedback uses the previous 64-sample output block and includes the
configured dry/wet mix. Loop boundaries use a short fade. In varispeed, speeds other than
1× also change pitch. Pitch and stretch modes read the loop with two grain heads (~65 ms,
half a grain apart, triangular crossfade): in pitch mode the play position moves at 1× and
the heads at the speed; in stretch mode the play position moves at the speed and the heads
at 1×. The grains are computed every second audio sample and interpolated, since the
loop holds nothing above ~2.4 kHz. During overdub, the live input is mixed at -6 dB with the existing loop
so new material is heard immediately; it is averaged and written once per low-rate
buffer position. In playback the loop replaces the live input, unless `looper_live_input`
(shared, 0/1, configuration page or Loop Monitor; saved, off by default) is on: then the input
is mixed in at the same -6 dB without being recorded, on both sides. After the resonator
(`looper_post`, per side, 0/1, CC 88 or Loop Monitor; runtime only) the input is that side's
output channel and always passes at unity, in every state.

In gate mode (`looper_gate`, per side, 0/1, CC 89 or Loop Monitor; runtime only) playback is
multiplied by a gate that opens while panel keys (not in Cave mode), MIDI notes on the side's
channel (or the Both channel) or the note looper's replay hold it, ramping in ~1 ms. Each new
key or note asks for a restart: the gate fades to silence (~2 ms), the play position jumps to
the loop start (the end in reverse) and the grain heads are re-placed. Only playback is gated;
overdub stays audible. Notes taken by MPE do not gate the loop.

Limitations: continuous speed (beyond the panel, Loop Monitor and norns steps) and
cross-feedback still require MIDI (SOS can also be set from the Loop Monitor, and loop level
over serial with `looper_level`, as the norns script does); there is no MIDI clock
synchronization, audio persistence, or automatic pitch modulation.

## Loop Monitor

`Tools/loop_monitor.html` shows both loopers live over USB serial (`loop_status`): audio
loop waveform, position, length, speed, level, direction and SOS, the note loop, panel
keys and controls, the looper latch (`latch` 0–2 in `loop_status`: off, on, on and locked), live input during playback (`live` 0/1), and input/output levels (scrolling waveform plus a dot meter, -48 to 0 dB with a 1 s peak dot, red while clipping). Its only controls are each side's Mode select (`set_param` `mode`, 0–18; options above 4 hidden
without the `resonator_modes` capability in `hello`), Pan and
SOS sliders, speed mode buttons (`set_param` `speed_mode`, 0–2) and speed step buttons
(`set_param` `speed_step`, 0–7), After resonator and Gate toggles (`set_param`
`looper_post` and `looper_gate`, 0/1; `loop_status` `a[9]` and `a[10]`; hidden without the
`looper_routing` capability), and the Looper latch and Live input in playback checkboxes
(shared `looper_latch` and `looper_live_input`), written with `set_param`. Open it from disk in desktop Chrome or Edge.
Pan and SOS require v4.30-duallooper.6 or later and the speed steps v4.30-duallooper.7 or
later (advertised as the `speed_steps` and `speed_modes` capabilities in `hello`), and the looper
latch v4.30-duallooper.8 or later (hidden when `loop_status` has no `latch`) and live input
v4.30-duallooper.9 or later (hidden without `live`); the rest works from v4.30-duallooper.5.
The looper latch itself (boot B+B, the configuration page field) is new in v4.30-duallooper.8,
live input during playback in v4.30-duallooper.9, the resonator modes in v4.30-duallooper.10,
the loop position and gate mode in v4.30-duallooper.11.

Two per-side `set_param` names have no Loop Monitor control but are used by the norns
script (below): `looper_state` (as CC 80: 0 bypass/stop, 1 new recording, 2 playback,
3 overdub, 4 clear; values outside 0–4 are refused with `invalid_parameter`, so a bad value
can never clear a loop) and `looper_level` (as CC 82: 0–1, stored in 1/127 steps in the
same variable the panel's G#/A# level steps use). Both are runtime only, new in
v4.30-duallooper.11 and advertised as the `looper_state` and `looper_level` capabilities in
`hello`.

## norns

[`Tools/norns/wingie_monitor`](Tools/norns/wingie_monitor/README.md) is the Loop Monitor on
norns, over the same serial protocol: both loops on the screen, record / play / overdub /
stop on K2 and K3 (`looper_state`), speed on E3, and the Loop Monitor controls plus loop
level (`looper_level`) and line input mono/stereo on a CTRL page. Opening the port from
norns does not reset the Wingie2, so loops recorded before connecting are kept.

## CPU headroom

The Faust resonators use about 80% of the audio task's block time, and audio glitches
appear near 92%. The loop engine therefore processes whole 64-sample blocks with the
hot state in locals, decodes stored frames only when the read position moves, and
computes the speed step once per block. The engine header is compiled at `-O2` (the sketch
default is `-Os`), and the plain and grain paths are separate template instances. With
both sides overdubbing, measured averages are about 293k cycles per block in varispeed
and 308–310k in pitch or stretch at 2× (budget 348k, glitches near 320k). Building with `-DLOOPER_PROFILE=1` (for example
`--build-property compiler.cpp.extra_flags=-DLOOPER_PROFILE=1`) prints a `LOOPER_PROF`
line every two seconds with average and peak cycles per block, total and looper-only,
against the budget.

## Build

Follow `AGENTS.md`: install ESP32 Arduino Core 2.0.4, MIDI Library, and Adafruit
AW9523, then run:

```
arduino-cli compile --fqbn esp32:esp32:esp32 --libraries Libraries Wingie2
```

The looper is not part of the Faust DSP: it is implemented in the C++ architecture
through `LooperDSP`. The only looper-era change to `Wingie2.dsp` is the post-resonator
pan (`pan0`/`pan1`). If `Wingie2.cpp` is regenerated with Faust, reapply the
`looper_dsp.h` include, wrap `fDSP` after it is created, allocate the looper before
`fAudio->start`, and restore the hand-written `Wingie2` methods declared in `Wingie2.h`:
`setLooperControl`, `setLooperRetention`, `setLooperSpeed`, `setLooperSpeedMode`, `takeLooperFilled`, `takeLooperWrapped`,
`takeLooperProfile`, `looperView`, `looperPeaks` and `readLevels`.

A successful build is required before flashing. On the Wingie2, verify startup,
bypass, two different loop lengths, automatic playback at the end of the buffer,
overdub, clear, mode changes, MIDI, and stability at high decay settings. Each release is
flashed and checked on hardware over USB serial before publishing; the MIDI
CCs for pan (10/42) and SOS (83) added in v4.30-duallooper.6 have not yet been tested
from a MIDI interface.
