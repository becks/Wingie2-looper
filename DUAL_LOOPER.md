# Wingie 2 — Two Asynchronous Loopers (Prototype)

Each mono input feeds an independent looper followed by the original resonator for
that channel. The resonator modes, notes, and controls remain unchanged. The looper
is an additional processing layer, not a new selector mode. It can be controlled over
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

| CC | Function | Value |
|---|---|---|
| 80 | State | 0 bypass/stop; 1 new recording; 2 playback; 3 overdub; 4 clear |
| 81 | Speed/pitch | 0 approximately half speed; 64 normal; 127 approximately double speed |
| 82 | Loop level | 0–127 |
| 83 | Material retained during overdub | 0–127; default approximately 90% |
| 84 | Cross-feedback during overdub | 0–127 = 0–25% of the opposite output |
| 85 | Reverse | 0–63 forward; 64–127 reverse |

Send CC80=1 on channel 1, then CC80=2 to close loop A. Repeat on channel 2 with a
different duration for loop B. CC80=3 adds the live input and feedback to the existing
loop. CC80=0 preserves the loop but returns to the live input. Recordings are volatile
and disappear when the instrument is powered off.

Audio is stored as 8-bit G.711 μ-law at 4.9 kHz while the codec and DSP continue to run
at 44.1 kHz. Companding gives quiet and medium-level material more resolution than
linear 8-bit PCM without increasing memory use. Each stored sample is the average of
nine input samples, and playback is linearly interpolated. The measured 55,288-byte allocation provides approximately
5.64 seconds per side. PSRAM is not available on the tested unit. Recording automatically
switches to playback when the buffer is full. If allocation fails, the signal remains in
bypass. Cross-feedback uses the previous 64-sample output block and includes the
configured dry/wet mix. Loop boundaries use a short fade. Speeds other than 1× also
change pitch. During overdub, the live input is mixed at -6 dB with the existing loop
so new material is heard immediately; it is averaged and written once per low-rate
buffer position.

Prototype limitations: continuous looper parameters still require MIDI; there is no
MIDI clock synchronization, audio persistence, or automatic pitch modulation. This is
not yet a hardware-verified release.

## Build

Follow `AGENTS.md`: install ESP32 Arduino Core 2.0.4, MIDI Library, and Adafruit
AW9523, then run:

```
arduino-cli compile --fqbn esp32:esp32:esp32 --libraries Libraries Wingie2
```

The original Faust DSP is unchanged. Integration is implemented in the C++ architecture
through `LooperDSP`. If `Wingie2.cpp` is regenerated with Faust, reapply the
`looper_dsp.h` include, wrap `fDSP` after it is created, allocate the looper before
`fAudio->start`, and restore `setLooperControl`.

A successful build is required before flashing. On the Wingie2, verify startup,
bypass, two different loop lengths, automatic playback at the end of the buffer,
overdub, clear, mode changes, MIDI, and stability at high decay settings. The device
has not been flashed from this environment.
