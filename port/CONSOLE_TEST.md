# Console test: batch of 2026-11-12

Committed (8cff251..bb90d1c). Run on a console on 2026-11-12; the results are in the table and in
[STATUS.md](STATUS.md).

## 1. Copy to the Vita

- Install `port/vita/sh2vita.vpk` with VitaShell.
- Copy these two files from `pack-vita/3ds/sh23ds/` to the same paths under `ux0:data/sh2-vita/`:
  - `bg/ap/ap72.s3m`
  - `bg/ap/tex/0001088f.t3x` (new)

## 2. Before playing

Options → **Debug** → **Performance Log** On.

## 3. What to check

| # | What | Where / how | Expected | Result |
|---|---|---|---|---|
| 1 | Debug tab | Options → Debug | Six tabs fit in the tab row; each row switches live | Works |
| 2 | Shadow Volumes | Debug tab On, flashlight on | Shadows drawn in red | Works |
| 3 | Map Test / Fog Test | Change the views | The room or the fog changes as named | Works |
| 4 | West Apartments 2F floor | The save point room | Dark worn floor, not white with dark panes; closer to 30 fps | Works |
| 5 | Maria at the bowling alley | The scene where she stays outside | Maria visible for the whole scene | Works |
| 6 | Eddie and Laura | First scene inside the bowling alley; the talk with Eddie eating | Both there from the start; only one Eddie | Works |
| 7 | Flooded elevator | Hotel basement (otherworld) | Doors drawn open, close during the ride, stay closed after | Not fixed: looks closed, James walks through |
| 8 | Push on a blow | Get hit by any enemy | James slides back a little, never through a wall; none from grabs or guns | Works |
| 9 | Lying Figure | One that sees James while wandering | A short walk before it spits, not an instant spray | Misread: the issue is no pause between attacks (Mandarins, Nurses) |
| 10 | Audio | Item pickups, doors, gunshots, loud fights with music | No click at the end of sounds, no crackle on bass | Not fixed: crackle unchanged (White Noise, forest) |
| 11 | Prop shadows | Bars, furniture, crates in the flashlight | Intact shapes, also while James stands still | Red volumes fine; broken shadows aren't red (other geometry) |
| 12 | Maria's voices | Born From a Wish | Her lines play, two of them with subtitles | Later |
| 13 | Footsteps | Hotel rooms 0x92, 0x99, 0xa0, 0xa3; the flooded basement after draining; hospital rooms 0x2a, 0x46 | Different step sounds on those floors | Later |
| 14 | Arrivals | Rebirth ending back to the lake; hospital 1F otherworld elevator car; boarding the lake boat; dog ending | On the boat; on the car's floor; no fall; the movie, then the title | Later |

## 4. Logs to bring

| Issue | How to capture | File in `ux0:data/sh2-vita/` |
|---|---|---|
| Circle does not switch the flashlight | Press Circle a few times in free play | `perf.txt` (the `flashlight` lines) |
| The camera zooms in hard when a monster is near | Right after it happens: pause → Options → Debug → Save Camera Trace On | `camtrace.txt` |
| The game freezes on opening the map after a long session | Play with Performance Log on until it happens | `stall.txt` and `vita_log.txt` |
| A prop shadow is still broken | Shadow Volumes On, a screenshot (PS + START), and the room's name | the screenshot |

## 5. What the batch changed (for the commits)

| Change | Files |
|---|---|
| Flooded elevator doors open and close as on the PC (0x577600, 0x577690, 0x5777f0) | `prog_stagefn.c`, `prog_hotel.c`, `prog_int.h` |
| Push on a blow, a stand-in from the attacks' sp (the PC's own push is 0) | `combat.c` |
| Flashlight: the reason for an ignored press goes to the log | `main.c` |
| Audio: fade out of stopped channels, a limiter in place of per-sample clipping | `port/vita/compat/ndsp.c` |
| Lying Figure: the walk's loop count starts again on chasing (0x4c5070, 0x4c5230) | `enemy.c` |
| West Apartments floor: texture ids that clash between maps get their own | `tools/sh2conv/convert.py` |
| Scene characters always drawn, as on the PC (0x950be4 is never read) | `demo.c`, `stagemdl.c`, `stagemdl.h` |
| Born From a Wish: Maria's voices through 0x49fe30 | `prog_wish.c` |
| Per-room footsteps (0x54a9a0) | `sfx.c` |
| Prop shadows: one stretch length per volume | `shadow.c` |
| Map freeze: watchdog steps around the map texture | `map.c` |
| Options, Debug tab in place of the SELECT combinations; diagnostics off by default | `optmenu.c`, `optmenu.h`, `main.c`, `fog.c`, `fog.h`, `port/vita/compat/c3d_gl.c`, `vita.h` |
| Windows install steps, README trimmed | `README.md`, `.github/release-notes.md`, `port/DEBUGGING.md` |
