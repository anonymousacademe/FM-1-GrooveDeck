# GROOVEDECK manual

GROOVEDECK is a drum machine and groovebox for the M-VAVE FM-1: 16 synthesized drums (one per pad), a 16-step sequencer, 8 patterns, 4 kits,
a mixer, a ping-pong delay and a bit crusher. Package identity `FM-1_950`. This manual covers the panel in short and **MIDI** in full;
`STATUS.md` says what is verified on the hardware.

## 1. Panel

| Control | Function |
|---|---|
| Keys 1-16 | The 16 drums: KICK SNARE CLAP C.HAT O.HAT TOM RIM COWBL \| CNG H CNG L CLAVE SHAKE CRASH RIDE G.SNR ZAP. In **STEP** mode they are the 16 steps of the selected drum |
| Keys 17-24 | Pattern 1-8 (changes at the end of the bar while playing) |
| Keys 25 / 26 / 27 | MUTE / SOLO / CLEAR: hold, then press a pad (CLEAR + a pattern key clears the pattern) |
| **PLAY**, **REC**, **SEQ** | Start / stop, live record, step edit |
| **EDIT ENV FX GLO** | Pages: sound (4 knobs per drum), mix (level, pan, delay send, mute), FX, global |
| **ARP** held | Note repeat: the held pads roll at the REPEAT rate |
| **SEL** held + a pad, SELECT knob | Select a drum |
| **OCT- / OCT+** | Pattern down / up |
| ALGORITHM, PRESETS, MASTER | Tempo, kit (CYBER 909 808 DUST), volume |
| **SAVE** | Project to flash (patterns, sounds, mixer, tempo); done when stopped |
| **HOME** | Tap: sound page. Hold: silence everything and stop |

## 2. MIDI

### In: playing GROOVEDECK from a keyboard, pads or computer

There are two ways in, and the firmware cannot tell them apart (one queue):

- **The 3.5 mm TRS jack (MIDI in).** Connect a MIDI OUT (5-pin DIN) to it with a DIN-to-TRS cable or adapter of **type A**. The jack is an
  **input only**: the FM-1 has no MIDI out jack.
- **USB-MIDI** from a computer or phone (the FM-1's USB port is device-only; a USB keyboard or pad controller has to go through the computer or a USB host box).

**The TRS input is new here: the hardware code (`hal/fm1_uart.h`) is unchanged from the CYBERDECK / Felucca platform, but we have not tried it
on a GROOVEDECK unit yet. The parser is tested on the PC (`tests/midi.txt`). USB-MIDI in and out were not tried on the hardware for this release either.**

What it understands, on **any channel** (channel 10 is the usual drum channel):

| Message | What it does |
|---|---|
| Note on | Plays a drum (table below). The **velocity** is the hit level, 1-127 (the panel pads hit at 100). With **REC** armed and the sequencer playing, the hit is recorded on the nearest step (velocity 110 or more records an accent); with **ARP** held it starts a note repeat |
| Note off, or note on with velocity 0 | Drums are one-shots, so it does not cut a sound (the gated snare closes its own gate with its GATE knob). It ends a note repeat that the note started |
| Program change 0-7 | Selects the **pattern** 1-8 (while playing, at the end of the bar, like the pattern keys) |
| Program change 8-11 | Loads the **kit** CYBER, 909, 808, DUST (as the PRESETS knob does: it replaces the sounds and mixer of all drums) |
| CC 120, CC 123 | Silences the drums that are sounding. The sequencer keeps running (HOME held stops it too) |
| Clock, Start, Continue, Stop | Received and **ignored** (no sync to an external clock yet; GROOVEDECK keeps its own tempo) |
| SysEx, other messages | Ignored (SysEx on the jack is dropped, so the updater cannot be started from it) |

Panel pads and MIDI work together: MIDI notes always play drums, also in STEP mode (the panel pads edit steps there).
MUTE and SOLO apply to the sequencer only, as for the panel pads. Hits that arrive over MIDI are **not** sent back out.

**The note map.** Notes 36-51 (C2-D#3) are the pads 1-16 in the panel's order, the usual drum-pad layout:

| Note | 36 | 37 | 38 | 39 | 40 | 41 | 42 | 43 | 44 | 45 | 46 | 47 | 48 | 49 | 50 | 51 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Drum | KICK | SNARE | CLAP | C.HAT | O.HAT | TOM | RIM | COWBL | CNG H | CNG L | CLAVE | SHAKE | CRASH | RIDE | G.SNR | ZAP |

General MIDI drum notes **outside** that range play the drum they clearly match:

| Notes | Drum |
|---|---|
| 35 (acoustic bass drum) | KICK |
| 52, 55, 57 (Chinese, splash, crash 2) | CRASH |
| 53, 59 (ride bell, ride 2) | RIDE |
| 54, 69, 70, 82 (tambourine, cabasa, maracas, shaker) | SHAKE |
| 56, 67, 68 (cowbell, agogo) | COWBL |
| 58 (vibraslap) | ZAP |
| 60, 62, 63 (hi bongo, mute and open hi conga) | CNG H |
| 61, 64 (low bongo, low conga) | CNG L |
| 75, 76, 77 (claves, wood blocks) | CLAVE |

Inside 36-51 the pad order wins over General MIDI (so GM's snare, note 38, is CLAP here: use note 37 for the snare, or map your pads).
All other notes do nothing.

### Out: GROOVEDECK as a MIDI source (USB)

Over **USB-MIDI, channel 10**, GROOVEDECK sends every drum that sounds from

- the **panel pads** (not in STEP mode, where they edit steps), including **note repeat**;
- the **sequencer's steps**: a drum that is muted (or not soloed) does not sound and is not sent.

as a **note on** with the same note map (reverse: drum 1 = 36 ... drum 16 = 51) and the hit level as velocity (the sequencer sends 127 for an
accent, else the level set by ACCENT), followed by a **note off 30 ms later**. A drum hit again within 30 ms gets its note off first.

Not sent: hits that came in over MIDI (no echo), clock, program changes, panic. There is no MIDI out jack, and no MIDI clock out yet.

### Trouble

- Nothing plays from the TRS jack: check that the cable or adapter is type A (a type B one swaps the wires), that it goes to the keyboard's MIDI OUT, and
  try USB-MIDI to tell a cable problem from a firmware one. The FM-1's TRS input has not been tried by us on this firmware.
- Wrong drums: your controller's drum notes are General MIDI, which differs from the pad order here for notes 36-51 (see above).
- Notes out do not arrive: they only go over USB, and only while the FM-1 is connected to a computer that has the MIDI port open.

## 3. Host tests (for developers)

`.\fm1.ps1 host groovedeck tests/midi.txt > build\host\midi-trace.txt` then `python tests\check_midi.py build\host\midi-trace.txt` runs the MIDI
scenario on the PC: raw bytes on the TRS jack (script command `T uart B0 B1 ...`), running status, real-time bytes inside a message, SysEx,
velocity 0, every channel, the General MIDI notes, program change, CC 120/123, note repeat, live record, and the notes that go out.
