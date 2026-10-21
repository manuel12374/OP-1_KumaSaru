# monolith — techno / industrial drone engine for the OP-1 (op1r)

An engine for long drones that never repeat, made to sit under techno and progressive tracks.

## Knobs
- **blue – tone**: filter opening, from dark to bright (50 % key tracking)
- **green – motion**: how much and how fast the drone evolves (0 = static)
- **white – rust**: inharmonic ring mod, wavefolder and hiss (clean to industrial)
- **red – thickness**: detune of the 3 saws and level of the sub one octave down

Tip: on the T2 page set long attack and release; hold one or two low notes (or a sequencer in hold).

## How it works
3 saws + square sub → ring mod with a sine at a drifting non-integer ratio → wavefolder → noise →
resonant state-variable filter → soft clip. Cutoff, resonance, beating speed, ring ratio and a slight pitch drift
are moved by 3 slow LFOs with incommensurate periods plus 3 random walks, so the combination practically never
repeats. The modulation state keeps running from one note to the next.

## Screen
After *2001: A Space Odyssey*: a monolith in the middle and four hominids, one per knob in its colour, walking
towards it as the knob goes up and touching it at the top. Moons = the live LFOs, blue line = the live filter,
white scratches = rust, red depth edge = detune.

## Testing on a PC
    python3 ../../harness/audio.py monolith                      # official harness test (sanitizers)
    ./render_demo.sh dark 90 7000,14000,4000,20000 7000,14000,4000,20000 24,36
    python3 ../../harness/render.py monolith                     # screen preview (PNG)

## Status
Runs on the OP-1 with op1r 44. Audio and screen are tested on the PC with ASan + UBSan (no overflows, no clicks).
The plugin file is built with bfin-elf gcc 12.3 (the same toolchain builds op1r's own plugins byte-identical).
`order` is 99 in `plugin.json`: plugin files get their number from the loader; a final number would be assigned
by the op1r maintainers if the plugin is submitted upstream.
