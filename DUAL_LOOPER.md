# Wingie 2 — Two Asynchronous Loopers

Each mono input feeds an independent looper followed by the original resonator for
that channel; each side's output (resonator plus dry signal) then goes through a stereo
pan (see the README; by default left stays hard left and right hard right). The resonator
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
While holding both Mode buttons, F selects 0.5× speed, F# selects 1×, and G selects
2× on the keyboard's corresponding looper. G# lowers and A#/B♭ raises that side's
loop level in approximately 6% steps. B toggles reverse playback.

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
String and Bar set the note without changing the tap sequence, and Cave mode neither
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
4 KB chunk pairs (one chunk per side) spread across the internal-RAM heap regions,
leaving 32 KB of heap and a 16 KB contiguous block for later allocations. The firmware
links neither WiFi nor Bluetooth (not even `btStop()`), so the core releases the
Bluetooth controller reserve and the WiFi statics are not included. The measured
172,032-byte allocation (21 chunks per side, measured on v4.30-duallooper.6) provides
approximately 17.55 seconds per side; the boot log prints the actual figure on a
`LOOPER_MEM allocated=` line. PSRAM is not
available on the tested unit. Recording automatically switches to playback when the
buffer is full; that side's LED then blinks red/white for about 0.75 s. During playback
and overdub, the LED flashes purple (white in Cave mode) for about 120 ms each time the
loop restarts. If allocation fails, the signal remains in bypass. Cross-feedback uses the previous 64-sample output block and includes the
configured dry/wet mix. Loop boundaries use a short fade. Speeds other than 1× also
change pitch. During overdub, the live input is mixed at -6 dB with the existing loop
so new material is heard immediately; it is averaged and written once per low-rate
buffer position.

Limitations: continuous speed and level (beyond the panel steps) and cross-feedback still
require MIDI (SOS can also be set from the Loop Monitor); there is no MIDI clock
synchronization, audio persistence, time stretching, or automatic pitch modulation.

## Loop Monitor

`Tools/loop_monitor.html` shows both loopers live over USB serial (`loop_status`): audio
loop waveform, position, length, speed, level, direction and SOS, the note loop, panel
keys and controls, and input/output levels. Its only controls are each side's Pan and
SOS sliders, written with `set_param`. Open it from disk in desktop Chrome or Edge.
Pan and SOS require v4.30-duallooper.6 or later; the rest works from v4.30-duallooper.5.

## CPU headroom

The Faust resonators use about 80% of the audio task's block time, and audio glitches
appear near 92%. The loop engine therefore processes whole 64-sample blocks with the
hot state in locals, decodes stored frames only when the read position moves, and
computes the speed step once per block. Building with `-DLOOPER_PROFILE=1` (for example
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
`setLooperControl`, `setLooperRetention`, `takeLooperFilled`, `takeLooperWrapped`,
`takeLooperProfile`, `looperView`, `looperPeaks` and `readLevels`.

A successful build is required before flashing. On the Wingie2, verify startup,
bypass, two different loop lengths, automatic playback at the end of the buffer,
overdub, clear, mode changes, MIDI, and stability at high decay settings. Each release is
flashed and checked on hardware over USB serial before publishing; the MIDI
CCs for pan (10/42) and SOS (83) added in v4.30-duallooper.6 have not yet been tested
from a MIDI interface.
