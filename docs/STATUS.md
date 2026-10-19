# GrooveDeck: where we left off

Drum machine / groovebox for the M-VAVE FM-1. Package identity `FM-1_950`. Flashed and running on the FM-1 as of this commit.
**Not yet verified on the hardware:** how the drums sound, the CPU figure on the bottom line, and the first project SAVE
(it uses a new flash sector, 0xFB000).

## What works (v0.2, PC-tested; built and flashed)

- 16 synthesized drums, one per pad: KICK SNARE CLAP C.HAT O.HAT TOM RIM COWBL | CNG H CNG L CLAVE SHAKE CRASH RIDE G.SNR ZAP.
  Four knobs each. The closed hat chokes the open hat. Four kits (CYBER, 909, 808, DUST) on the PRESETS encoder.
- 16-step sequencer, 8 patterns (queued to the end of the bar while playing), swing, accent, live record, note repeat (hold ARP).
- Mixer per drum (level, pan, delay send, mute), solo, clear. Ping-pong delay synced to the tempo, bit crusher, output limiter.
- Project save: patterns, sounds, mixer, tempo to flash sector 0xFB000 (`project_load/save` added to this project's `platform.c`). It saves
  when stopped.
- UI: step grid (8 drums per bank, the bank follows the selected drum), a big banner when the selected drum changes, parameter bars.

## MIDI (added after v0.2, PC-tested only)

TRS-jack MIDI in (`firmware/src/midi_uart.c`, platform `FELUCCA_UART 1`), USB-MIDI in, USB-MIDI out on channel 10. Notes 36-51 = pads 1-16, plus
General MIDI drum notes outside that range; velocity = hit level; program change 0-7 pattern, 8-11 kit; CC 120/123 silence the drums. Out: pad hits and
sequencer steps, note off after 30 ms, nothing echoed. Manual: `docs/MANUAL.md`. Test: `tests/midi.txt` + `tests/check_midi.py`.
**Not tried on the hardware:** the TRS jack input, and USB-MIDI in/out with this firmware (not flashed since).

## Controls

Keys 1-16 pads (or steps in STEP mode) | 17-24 pattern | 25 MUTE, 26 SOLO, 27 CLEAR (hold + pad) | SEQ step mode | PLAY REC |
EDIT sound, ENV mix, FX, GLO global | ARP held = note repeat | SEL held + pad = select a drum | OCT-/OCT+ pattern | SAVE |
HOME tap = sound page, hold = panic and stop | SELECT knob = drum | ALGORITHM = tempo | PRESETS = kit.

## Ideas / next

- Per-step page on the LFO button: probability, ratchet, accent, micro-timing.
- Fill (key 27 is Clear for now).
- 32-step patterns (OCT-/OCT+ would page the steps in STEP mode), pattern chaining.
- MIDI clock in/out (the clock bytes are received and ignored now).
- Tune by ear: hats, cowbell and clave levels; kit values are estimates.
- Grid row labels, or a bank letter, so rows can be read without selecting them.

## Developing

From the parent folder (`FM-1 Projects`): `.\fm1.ps1 host groovedeck tests\<script>.txt`, then `build groovedeck`, then `install groovedeck`.
Host test scripts live in `tests\` (`pads16.txt` plays every drum, `groove.txt`, `features.txt`, `select.txt`, `pat23.txt`).
The host harness prints every drum hit with its time, which is how timing is checked.
