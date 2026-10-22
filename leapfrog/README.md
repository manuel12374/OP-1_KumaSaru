# Leapfrog — a random note generator for the OP-1 (op1r)

Every parameter is a **range**, drawn as a log floating on a river. At each note a frog on every log jumps to the
value drawn for that note, so you can watch the dice being rolled. Works in synth and drum mode. 8-bit arcade look.

![animation](../demos/leapfrog_anim.gif)

## Rows
| row | encoder | range |
|---|---|---|
| RATE | blue | time to the next note, 1/64 .. 16 bars |
| GATE | Shift + blue | note length, 1/64 .. 16 bars |
| NOTE | green | pitch range C1 .. C7, taken from the scale |
| JUMP | Shift + green | leap from one note to the next, 0 .. 12 scale steps (up or down; reflected at the range edges) |
| VEL | white | velocity 1 .. 127 |
| AIR | Shift + white | chance of a rest, 0 .. 100 % |
| MEM | red | memory: chance that a note repeats the phrase (100 % = a loop, 0 % = always new); press: phrase 4 / 8 / 16 / 32 |
| POLY | Shift + red | notes per hit 1 .. 4 (chords of stacked thirds) |

**Turn** an encoder to move the range (its centre, the target). **Press** it to edit the width (arrows appear at the
log's ends; the name is underlined): turning then makes the range longer or shorter around its centre. With Shift
held, turn / press work on the Shift row. AIR and MEM are single values: their log grows from the left.

On AIR and MEM the frog stands on the log when it happened (a rest, a note from the memory), else it swims with only
its head out of the water.

## Keys (leapfrog screen)
| key | does |
|---|---|
| note key | root of the scale (drum mode: the scale picks the pads) |
| drop / Shift + drop | next / previous scale: MAJ MIN DOR PHR PEN+ PEN- |
| lift | start / stop |
| scissors | new phrase (forget the memory) |

Any screen while leapfrog is the sequencer: Shift + F G A B = pattern 1–4 (all rows + scale), Shift + C# = start /
stop (with REC held: also starts the tape). Patterns are saved on the OP-1 (DB table `leapfrogseq`).

## Testing on a PC
    python3 test/render_leapfrog.py ../../op1r-src     # host test (sanitizers) + preview + animation

Checks: rates and the 1/16 grid, notes in the scale and range, JUMP 0 / exact leaps, AIR 0 / 100, MEM 100 looping
with its phrase length and MEM 0 drawing anew, POLY chords, gates, the 6-voice limit, nothing hanging after stop,
range move / width / clamps, root and scales, patterns, Shift + C#, MIDI start, drum pads, save / load.

## Credits
- Frog: [Font Awesome](https://fontawesome.com) "frog" icon, CC BY 4.0, rasterised by `tools/gen_assets.py`.
- Water: the "water-broken" line icon, tiled by `tools/gen_assets.py`.
