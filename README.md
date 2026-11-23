# VILNIS 0.4 (Digitakt MKI, OS 1.54)

*Vilnis* is Latvian for wave. A west coast machine for the Digitakt MKI, inspired by Majella
Audio's Implexus and Compax and by the Buchla Music Easel: a complex oscillator (a modulation
oscillator at a ratio, FM, AM or both) into a sine wavefolder with HARMONICS and BIAS, then a low
pass gate with a vactrol-like decay, a pulser that strikes it again and again, a random voltage
and a vibrato. Pick it with **FUNC + SRC**. The Digitakt's filter, amp, LFO and FX follow, as on a
sample.

## Page 1: VOICE

| Knob | |
|---|---|
| TUNE | pitch |
| HARM | the main oscillator's shape: sine, through triangle, to saw |
| FOLD | how hard it's folded. Its picture is the folded wave |
| BIAS | the folder's symmetry: off-centre folding brings in even harmonics |
| RATI | the modulation oscillator's pitch against the main one, ×1/4 to ×8 |
| MOD | how much the modulation oscillator modulates |
| DCY | the low pass gate's decay: short plucks to long rings |
| LEV | level |

## Page 2: WEST (press SRC again)

| Knob | |
|---|---|
| MODE | **FM**, **AM** or **BOTH** |
| FINE | the modulation oscillator's detune, ±1 semitone: off-ratio for bells and clangs |
| GATE | the low pass gate: **OFF**, **LP** (it darkens as it dies), **VCA** (it fades) or **BOTH**, the classic Buchla bongo |
| RES | the gate's resonance |
| PULS | the pulser: strikes the gate again and again, **OFF** or 0.25–32 Hz |
| RAND | a random voltage, new on each strike, added to FOLD and MOD |
| VIB | a vibrato. Its picture, with FOLD's, shows the waveform and the gate's level |

## Playing it

- Start with GATE on **BOTH**, a short DCY and a little FOLD: a woody, plucked Buchla tone.
- Turn up **PULS** for repeating strikes that follow the gate, and **RAND** for each one to come
  out different.
- Integer RATI values sound harmonic. FINE pulls them off into bells and metal.

## Install

Load `vilnis-0.4.elemod` in Elekloader with **core 4.4** and **machine-pages** (1.11), build your OS
and flash it. No `.syx` is shared here: it would contain Elektron's firmware.

## Notes

Needs **core 4.4** and **machine-pages**. Machine id 36. Page 2 uses sound slots 46–52, like every
machine-pages machine with a second page, so on a VILNIS track don't use FX Locks or Deja Vu's RND
destinations.

`tables.h` (the sine, the gate's decay times and the pulser's rates) is made by `tools/gen_vilnis.py`.

Licence: GPL-2.0-or-later. By Cere, made with Claude. Not affiliated with Majella Audio or Buchla.

## Changes

| Version | What changed |
|---|---|
| 0.4 | The screen code is built smaller, which leaves more RAM for your other mods. The sound is unchanged. |
