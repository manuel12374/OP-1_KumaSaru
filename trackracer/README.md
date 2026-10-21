# Track Racer — a circular sequencer for the OP-1 (op1r)

After Google's *Racer* experiment with Giorgio Moroder: six concentric lanes, a little car on each, and a lane plays
when its car drives over a **stop sign**. Works in synth and drum mode.

![animation](../demos/trackracer_anim.gif)

## How it works
- **6 lanes** (the OP-1's 6 voices), lane 1 the outer one. One note at a time per lane.
- **Lap length per lane**: 1 2 3 4 5 6 7 8 12 16 beats, so the cars drive at different speeds and the lanes phase
  against each other (4 against 3, 6 against 8…).
- **64 positions per lap.** Each stop sign keeps its own note: in drum mode a drum sound (+ pitch), in synth mode a
  pitch, so a lane is usually one sound but can also play a line.

## Controls (track racer screen)
| control | does |
|---|---|
| blue turn | select lane 1–6 |
| green turn | cursor along the lane (Shift: 4 positions = 1/16 of the lap) |
| white turn | lap length · **press**: gate (1/32 1/16 1/8 1/4 1/2 1/1 TIE) |
| red turn | right = drive, left = park the selected car · Shift: all cars · **press**: lane level |
| REC | record on / off |
| note key | REC on, car parked: a sign at the cursor · car driving: **live**, a sign where the car is (nearest position, it does not double-trigger) · REC off: just plays |
| scissors | delete the sign under the cursor · Shift: clear the lane |
| lift / drop | copy / paste the lane · Shift: the whole pattern |

Any screen, while track racer is the mode's sequencer (same shortcuts as euclid / dots):
Shift + F G A B = pattern 1–4 (cars keep their place on the circle) · Shift + G A B C D E = mute lane 1–6
(the car keeps driving) · Shift + D# = mute all · Shift + C# = start / stop all (with REC held: also starts the tape).

Everything is saved on the OP-1 (DB table `trackracerseq`); cars come back parked.

## Screen
Lanes as concentric circles with a chequered start / finish line; each car in its colour where it is on its lap
(parked: dim, on the start line); stop signs as red octagons with the white band on the selected lane and red dots on
the others, flashing white when played; muted lanes dashed. Right: lane, cursor + the sign's note, lap / gate,
DRIVE / PARK / level, number of signs, pattern, REC / LIVE.

## Testing on a PC
    python3 test/render_trackracer.py ../../op1r-src     # host test (sanitizers) + screen preview + animation

The host test checks timing for every lap, polyrhythms, gate and TIE, nothing left hanging, live record (nearest
position, no double trigger), step record, delete / clear, mutes, patterns and lap changes keeping the car's place,
copy / paste, start / stop shortcuts, MIDI start and the save / load round trip.
