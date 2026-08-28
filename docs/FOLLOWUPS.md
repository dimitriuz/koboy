# koboy — what is still open

Deferred findings: real, judged real when they were found, and not yet done.
koboy now runs fifteen systems on one verified device, and the owner has
played on it — the cores, the controls, the in-game MENU, cartridge saves and
save states have all been exercised by hand. Everything that was only ever
"nobody has run this on hardware" is therefore gone from this file, along with
everything marked CLOSED; the retired numbers are indexed at the bottom so a
`docs/FOLLOWUPS.md #N` in a source comment still resolves.

**Numbers are stable identifiers and are never reused.** `src/`, `tests/`,
`TESTED.md`, `INTERNALS.md` and `CLAUDE.md` all cite them, so the sequence has
gaps and that is deliberate — a tidy sequence is worth less than a citation
that still points at the thing it was written about.

**The ordering is by subsystem, not by age.** The old "what bites first" rule
stopped describing the file once it grew past thirty entries and four
sessions; a reader arrives here asking about a *part* of koboy, not about a
date. The handful that actually bite are listed immediately below.

Two claims that look the same and are not, because this file has confused them
before: **"has not run on the device"** is answered — the owner has played.
**"no automated test exists"** is not. A thing verified once by a hand on a
panel still has no regression test, and those entries are grouped under
"Coverage gaps" rather than deleted.

## Start here

- **#23** — `video_submit` is the bottleneck on every one of the fifteen
  systems, and nothing has optimised it. Every other performance entry is
  downstream of this one.
- **#84** — one file in the owner's collection segfaults the process. koboy has
  no answer to a core that dies inside `retro_run`, and #72 says twelve cores
  have never been swept for the same thing.
- **#92 / #95** — two SIGSEGVs in the owner's own log that have never been
  explained or reproduced, on a code path a remote session cannot exercise.
- **#25** — scroller smearing is substantially fixed by 1-bit output and is
  not solved. It is the oldest open defect here.
- **#78** — nine systems auto-fit with no measured scale ceiling, and two of
  them look exactly like the ones that turned out to need one.
- **#61** — an arcade save state can be 145 MB and nothing checks before
  allocating it on a 512 MB device.

## The picture: the pipeline, the panel, and what they cost

### 23. `video_submit` is the real optimisation target, not "presentation."

2026-08-26 device session, Zelda at scale 5, `present_divisor = 3`,
per-stage means: core 2.3 ms, **submit 17.0 ms**, blit 2.8 ms, refresh
0.4-0.75 ms (max 29.2 ms, the fixed cost + unreliable-timing spread
`TESTED.md` already documents). `video_submit` -- the RGB565->gray LUT,
integer scale, quantise and 8x8-tile diff, `src/video.c` -- dominates the
other three stages combined by roughly 5x. That contradicts the v1
design spec §5's stated premise ("Emulation is cheap; presentation is
the entire bottleneck"): `video_submit` is neither emulation nor
presentation (panel refresh) in that dichotomy, it is the pixel pipeline
sitting between them, and it is the bottleneck. Confirmed pixel-bound by
a render-scale sweep (submit time only): scale 3 / 207,360 px / 8,997
µs, scale 4 / 368,640 px / 12,462 µs, scale 5 / 576,000 px / 16,639 µs --
linear fit `submit ~= 4.7 ms + 20.7 ns/px` predicts the scale-4 point
within 1%. v2-core's multi-rect work (§7 of the v2 design spec) reduces
`refresh`, which this measurement shows is already the cheapest of the
four stages -- the optimisation that would actually move the needle is
in `video_submit`'s per-pixel work, unstarted.

### 25. Full-screen scrollers smear — substantially fixed, not solved

**Status, 2026-08-27: 1-bit output shipped as the default and the owner
confirms it fixes the smearing** (`TESTED.md`, "1-bit output fixes the motion
smearing"). This entry stays open because "substantially" is the honest word:
the mechanism below explains why a four-level picture on a two-level waveform
could never be erased cleanly, and 1-bit output removes the intermediate
pixels rather than making the panel faster. What it costs is priced in
`TESTED.md`'s refresh table — clean transitions at 153.5 ms a full-rect
refresh against DU4's 24.1 ms, a factor of 6.4, which is what area-aware
pacing (#100) now paces to. Anyone tempted to revert `force_dither` should
read the diagnosis first.

**The artifact was PHOTOGRAPHED, and a photograph says two things a frame
counter cannot.** The
owner filmed Super Mario Bros. on the panel. A jump leaves a vertical column
one dirty-rect wide holding three things at once:

- a faint grey ghost of an older position, ABOVE the sprite
- the solid current sprite
- **bright WHITE bands BELOW, brighter than the surrounding sky**

The white is the part that reframes this. It is not leftover sprite — it is
the panel driving pixels that were black toward the sky's mid grey and
overshooting past it. So **both directions of the transition land wrong**: the
old position is not fully erased, and where it is erased it goes too far. The
residue is confined to the dirty rect's column, so the dirty-rect logic is
doing its job; this is a waveform/level problem and nothing else.

**The measurement that sharpens it.** Sampled off the live framebuffer while
the game was on screen — what koboy WRITES, before the panel does anything:
sky 170 (level 2), brick 85 (level 1), cloud 0 (level 0). **The sky is a MID
GREY, and that is what makes every sprite transition a hard one.** A fast
waveform drives toward black or white; from level 2 it has to travel most of
the way in either direction and, at partial-refresh speed, lands somewhere in
between. A sky at level 3 would make a dark sprite over it a black<->white
transition, which is the one transition a two-level waveform completes exactly.

So there are TWO levers, and they compose:

1. **Make the CONTENT two-valued** — `force_dither`. Then no pixel ever asks
   for an intermediate state at all.
2. **Make the BACKGROUND land on an extreme** — which is the tone mapping's
   choice. See #96: measured through koboy's own reduction, `value` is the
   only shipped mapping that puts SMB's sky at pure white, and it does so by
   putting the HUD text there too (#48). Lever 2 is real and is NOT currently
   reachable without losing something else.

Lever 1 shipped this session, together with the two-level waveform it is
meant to pair with (`waveform_fast = du`, new) and one in-game row that
cycles the pair: **MENU -> MOTION**, rungs `4 GREYS / AUTO`, `1-BIT / AUTO`,
`1-BIT / DU`. **The visual claim is UNTESTED.** A `--frames` run cannot see
ghosting — residue is panel-side and koboy's dirty diff only ever compares
koboy's own output buffers — which is why this defect has outlived two
attempts at it. What was verified is that the content really is two-valued end
to end, that both halves reach the live pipeline and the live backend, and
that both keys persist. Whether it LOOKS better is the owner's call, on the
panel.

Why this is not the forced-DU4 experiment again: DU4 is the FOUR-level
variant, and FBInk's header says a DU-class waveform leaves on-screen pixels
as-is for new content that is not black or white. Against four-level output
that clause means the two middle levels are pixels the panel declines to
touch — which is what "cannot erase" looks like from the inside. Against
1-bit output there is no such pixel. Neither half of the pair is expected to
be worth anything alone, which is why the ladder's middle rung (1-bit under
AUTO) exists: it is the control that says whether the dithering alone did it.

**Also worth reporting back:** the ghost persists across several video frames
at 10 fps, so residue is accumulating over successive partial updates rather
than arriving in one bad one — consistent with #26, and the owner is already
at `present_divisor = 8`, the top of that ladder. The two settings interact
and should be judged together, not one at a time.

**The original shape of the defect, kept because it is the thing 1-bit output
displaced and the thing a revert brings back.** Not a regression: the player
confirmed v1 looked the same. On a horizontal scroll the picture degraded into
heavy horizontal streaking within a few seconds — a fast non-erasing waveform
draws each frame's background without clearing the last, and a scroll offsets
that background horizontally every frame, so successive frames superimpose.
`waveform_fast = auto` solved it for Tetris (small changed region, controller
picks an erasing waveform) and never for a scroller, where nearly the whole
rect changes and the controller still picks a fast one.

The lever tried before dithering was `full_refresh_permille`, which promotes a
frame to a flashing erasing refresh once the changed area crosses a threshold.
It ships at **1000 — tuned on Tetris, where it essentially never fires** — and
that value was carried into v2 unexamined. It is still unexamined, and it is
now the *second*-best lever rather than the only one: 400 was tried once with
the result unrecorded, and re-enabling `cleanup_interval` was the other
candidate. Both are policy questions about when to spend a flash, which is why
the v1 design spec's prediction ("Full-screen scrollers get no benefit and hit
the worst case") was right about the mechanism and wrong about the remedy.

### 26. `present_divisor` trades frame rate directly against smearing

Measured on the device, Darkwing Duck, 600 core frames; the run that added 4
through 12 reproduced 1, 2 and 3 to the frame against the earlier 2026-08-26
run, which is what makes all seven rows comparable. The setting is in the
in-game MENU as FRAMES.

| `present_divisor` | presented | fps | wall (ms) |
|---|---|---|---|
| 1 | 115 | 11.2 | 10263 |
| 2 | 102 | 9.9 | 10255 |
| 3 (shipped) | 76 | 7.4 | 10243 |
| 4 | 67 | 6.5 | 10243 |
| 6 | 49 | 4.8 | 10261 |
| 8 | 39 | 3.8 | 10243 |
| 12 | 31 | 3.0 | 10268 |

Wall clock is flat across a 12x range, so the emulation cost of this setting
is nil at every value -- what changes is only how many updates the panel gets.

THE CURVE IS NOT 1/d, AND THAT IS WHAT SET THE LADDER'S TOP AT 8. Delivered
frames fall much more slowly than requested ones, because koboy suppresses an
unchanged frame and a wider gap means fewer of the frames it does present are
duplicates: 8 -> 12 halves what is requested and removes only 8 presented
frames in ten seconds. Past 8 you give up pacing granularity for almost no
further reduction in panel updates, which is the only thing the setting exists
to reduce. `submit`'s mean rises with the divisor for the same reason (15.0 ms
at 3, 25.6 ms at 8): each presented frame carries more change.

**What is still open is the only part a measurement cannot settle.** Lowering
the divisor is free in emulation speed and costs *smearing* — more partial
updates a second means residue accumulates proportionally faster — so the
value is a judgement made while looking at the game, not a number read off a
frame counter. The owner's device runs 2, the shipped default is 3, and no
comparison of 4, 6 and 8 in motion has been written down. Since 1-bit output
and area-aware pacing (#25, #100) both changed what a presented frame costs
the panel, the old preference may not survive re-asking.

### 100. The settle model under-throttles mid-size updates by construction

Appendix E measured refresh duration as ~94% FIXED in area (DU: 145.1 ms at
160x144, 162.3 ms at 1120x1008). The pacing model is LINEAR in area, charging
`settle_full_ms * dirty/whole`. Those two do not agree in the middle: a
half-screen change is charged 75 ms and really takes about 149 ms.

That is deliberate and the reasoning is in `src/koboy.h` -- charging the true
fixed term to every update pins the device to 6.5 fps on static screens too,
which is no better than `present_divisor = 8` and loses the responsiveness the
owner currently has. But it means the fix is strongest exactly where the
complaint was (full-area scrolls) and weakest in between.

The lever is `settle_base_ms`, which ships at 0 and can go to 145. Nobody has
looked at a half-screen scene change on the panel with it raised. If the owner
reports flashing on scene transitions rather than on scrolls, that is this
entry.

### 96. Lever 2 — putting a flat background on an extreme — has no reachable answer

The MOTION pair itself has been judged: the owner played the three rungs and
1-bit output is now the default (#25). What did NOT get an answer is the other
lever the same investigation identified, and it is worth keeping because it is
the one that would help the Game Boy, which has the most to lose from
dithering and the least to gain — its four shades already ARE the panel's four
levels.

**Putting the sky on an extreme has no shipped mapping that does it without a
cost.** Measured through `video_rgb565_to_gray` +
`video_quantise4`, the shipped five, on SMB 1-1's own palette entries:

| colour | luma | bright | balanced | equal | value |
|---|---|---|---|---|---|
| sky `$22` rgb(92,148,252) | 143 L2 | 157 L2 | 167 L2 | 177 L2 | **255 L3** |
| brick `$17` rgb(200,76,12) | 107 L1 | 121 L1 | 116 L1 | 109 L1 | 206 L2 |
| Mario red `$16` rgb(216,40,0) | 90 L1 | 103 L1 | 101 L1 | 99 L1 | **222 L3** |
| ground `$27` rgb(228,160,104) | 176 L2 | 187 L2 | 183 L2 | 178 L2 | 231 L3 |
| cloud/white `$30` | 255 L3 | 255 L3 | 255 L3 | 255 L3 | 255 L3 |

`value` is the only one that lands the sky on pure white, and the same column
shows why it is not free -- it is max(R,G,B), so it lands MARIO on pure white
too, and the white HUD text with him (#48). The sprite and the sky would both
be level 3 and the picture would be gone. That is the whole of lever 2 today:
it is real, it matters (a level-3 sky turns every sprite edge into the one
transition a two-level waveform completes exactly), and no shipped mapping
delivers it without taking the foreground with it.

The owner's current `bright` and the shipped `balanced` differ by 10 grey
levels on this sky and land on the same panel level, so switching between
those two cannot move this at all -- worth saying plainly, because "gray is
ok" and "the sky's level is causing the smearing" are both true at once and
look contradictory.

A mapping that pushed only LARGE FLAT LIGHT AREAS to white would, but that is
not a per-pixel LUT any more -- it is spatial, and the LUT is one lookup in
the measured bottleneck (#23). Not attempted. Note also that lever 1 partly
subsumes it: under `force_dither` the sky is already only black and white
cells, so its exact level stops deciding whether transitions are completable
and starts deciding only how light the pattern reads.

### 99. Half-transitioning content has never been timed

Appendix E's waveform table is measured with a solid black/white flip -- 100%
of pixels transitioning, the worst case. Real content is not that: koboy's own
dirty diff measured Galaga at 67% and Galaxian at 86% of the game rect per
frame, and a dithered scroll changes about half its pixels rather than all of
them.

The probe DOES draw a phase-shifted checkerboard, but only through the
sustained path -- which the same session proved measures submission and not
completion, because the EPDC never applies back-pressure. So the checkerboard
numbers say nothing about settle time and the area-pacing model assumes a
half-transitioning region costs exactly what a fully-transitioning one does.

If it is materially cheaper, `settle_full_ms` is over-charging real content and
scrolling is choppier than it needs to be. Timing it needs a completion signal
this device's `unreliable_wait_for=1` makes untrustworthy, so the honest way in
is probably the same one that produced the sanity bracket: change the constant,
play, and look.

### 98. `waveform_fast = du` is now a rung that selects nothing new

Follows directly from #97. `MENU -> MOTION` cycles `4 GREYS / AUTO` ->
`1-BIT / AUTO` -> `1-BIT / DU`, and the last two are measurably the same
waveform on the content the first of them produces. The owner judged them
indistinguishable by eye before any of this was measured.

Not removed in the area-pacing task because that task had no mandate to change
the MOTION ladder and the 1-bit fix it guards is the one thing on this panel
nobody wants to disturb. But the rung costs a menu press and a config key to
express a difference that does not exist, and the ladder would read better as
two rungs. The counter-argument is a real one: AUTO's choice is the driver's
and could differ on a panel nobody has measured, so `du` is the only way to
pin it. If it stays, its ini comment should say that is what it is for.

### 24. `refresh_fixed_tiles` tuning (20 vs 40 vs 80 vs split-off) is inconclusive by construction, not just unmeasured

2026-08-26 device session, same Zelda run as #23, `--frames 900`: 20/40/80
all produced the same 339 rects over 292 frames (604 / 750 / 488 µs mean
`refresh`) -- behaviourally identical on real content, exactly as a host reviewer
predicted from the code before any device was available. Splitting off
entirely (100000) dropped to 292 rects / 368 µs, which is the expected
mechanical cost of one ioctl per extra rect, not evidence against
splitting. The reason none of this settles the tuning question: refresh
submission is non-blocking by design (see "What the hardware overruled"
in `CLAUDE.md`), so the in-process `refresh` timer measures submission
only, never the panel's actual asynchronous work -- which is what the
fixed-cost-per-rect model in the v2 design spec §7 is actually trying to
amortise. Measuring the real benefit needs blocking refreshes, and this
device reports `unreliable_wait_for=1`, which applies to exactly the
ioctl a blocking measurement waits on -- so those figures would be
suspect by construction too. `refresh_fixed_tiles` stays shipped at 40
(the untuned starting guess) with this recorded as a limit of the
measurement method, not a verdict on the split heuristic.

### 27. Multi-rect splitting shows no measurable benefit on real content

Six device runs, splitting on versus off, produced **identical** presented-frame
counts at every `present_divisor` (76/76, 102/102, 115/115), with rect counts
differing by two. Combined with #23 — `refresh` is ~0.4 ms of a ~23 ms frame
while `video_submit` is ~16 ms — v2-core's multi-rect work optimised a stage
that was never the constraint. Consider defaulting `refresh_fixed_tiles` to a
value that disables splitting until a workload is found where it pays.

### 21. `video_split_dirty`'s overflow fallbacks are untested

`src/video.c:245` (tile grid larger than `KOBOY_SPLIT_MAX_TILES`) and
`src/video.c:304` (more band/column candidates than
`KOBOY_SPLIT_MAX_CANDIDATES`) both degrade to the single merged rect and
are safe by construction, but nothing in `tests/test_video_multirect.c`
(or anywhere else) constructs a dirty pattern large or pathological
enough to actually reach either branch.

### 22. Emitted rects may partially overlap once the candidate list is capped

`src/video.c:313-323`. The merge-to-cap loop only removes a candidate that
ends up *fully contained* in another (documented in place as deliberate --
"does not attempt general deoverlap"); two capped rects can still
partially overlap, and every downstream consumer (`blit_gray8`, `refresh`)
redoes that overlap's area twice. Coverage is unaffected -- a union only
grows -- so this is a cost, not a correctness bug.

### 65. Two shape trades the pixel-aspect correction made, neither of them re-examined

Closing #51 changed how EIGHT systems are presented, not one -- NES, Master
System, Game Gear, Mega Drive, Atari 2600 and most FinalBurn Neo boards now
render at the aspect their cores report, alongside the four that were already
square. Every one was rendered and looked at on the host and every one
improved; `pixel_aspect = false` in `koboy.ini` is the way back if a system
ever looks worse for it. What was never re-examined is the two structural
choices inside the correction, and both were made on host renders:

- **The vertical scale stays an integer and only the width carries the ratio.**
  The alternative (fit the corrected aspect freely into the rect) fills more
  panel in some cases at the cost of uneven row replication -- a 250-row PAL
  frame at 2.88x draws rows 2 and 3 times alternately. With the rect now
  aspect-aware the integer rule wins on size too for everything measured
  (PAL Breakout is 1000x750, not the 667x500 it was before the rect knew), so
  there is nothing to change unless a real title shows otherwise.
- **The reserved rect is now `ceil(max_w * par)` wide.** For the Atari that is
  NARROWER than before (280 source columns instead of 320) and holds a bigger
  picture; for the NES it is wider (293 instead of 256). The faceplate is laid
  out around it, so these are the first systems whose DMG chrome differs in
  proportion from the Game Boy's. The chrome goldens cover the Game Boy and the
  LCD layout only.

### 30. The G&W cost comparison that looks alarming and is wrong

`video_submit` scales with **destination** pixels (~4.7 ms + 20.7 ns/px, #23).
The Game Boy is upscaled 160x144 -> 800x720 = 576k px = 16.6 ms. G&W renders
at 1x, so:

| Title | Canvas | dst px | est. submit | vs Game Boy |
|---|---|---|---|---|
| Parachute | 658x395 | 260k | 10.1 ms | 0.45x |
| Donkey Kong Circus | 498x771 | 384k | 12.6 ms | 0.67x |
| Mario Bros. | 973x532 | 518k | 15.4 ms | 0.90x |
| upper bound | 1073x777 | 834k | 22.0 ms | 1.45x |

Typical G&W titles are **cheaper** than the Game Boy, not 9-20x more
expensive. Comparing a G&W canvas to the Game Boy's 160x144 *source* is what
produces the alarming number, and the Game Boy never renders at 160x144.

Estimates, not measurements -- the constant comes from a host-era sweep and
every absolute timing this project has taken moved by up to 2.2x between
sessions.

### 41. Neo Geo Pocket is now the most expensive thing koboy renders

160x152 auto-fits to scale 6 on the Libra 2: a 960x912 rect, 875k destination
pixels, ~22.8 ms of `video_submit` by #23's model -- against the Game Boy's
measured 17.0 ms. On a Sage (1440x1920) it takes scale 7: 1120x1064, 1.19M
px, ~29.4 ms. Both are estimates from a linear model fitted on one device,
not measurements, and `present_divisor = 3` may absorb the difference
entirely. But it is the first system whose auto-fit lands materially ABOVE
the Game Boy's tuned cost, and if anything feels slow this is where to look
first. `scale = 5` in `koboy.ini` brings it back to 800x760 / 17.3 ms.

### 66. A core that changes ONLY its aspect mid-run re-fits; one that changes only its DAR while the fit is already at max does not re-scale the rect twice

`main.c` compares the pixel aspect the profile was resolved with against the
core's current one and re-resolves the whole rect when it moves, which is
correct and is exercised by `tests/smoke_host.sh`. What is not exercised is a
core that announces a new aspect for a geometry that is ALREADY at max in a
rect resolved for a different aspect: the rebuild path runs, so it should be
fine, but no core in reach announces an aspect change at all (measured: the
Game & Watch core, the only one that re-announces anything, reports
`aspect_ratio = 0` on all 59 titles). Nothing to do until a core does it.

### 101. The screenshot plaque's erase has never been watched on a panel

The cost question this entry was opened for is answered and its numbers are in
`TESTED.md` ("The in-game SCREENSHOT, on the device"): a capture costs one
frame about 6 ms longer than it would otherwise be, so it stays on the main
loop. The PNG decoded at 1264x1680 after transfer, so the hand-rolled
stored-DEFLATE writer works on the device too, and the FAT32 write path was
already proven there by cartridge SRAM.

**What is left is the half no instrument in this project can measure.**
`shot_note_rect` picks the band between the game rect and the controls, paints
a white plaque with `KOBOY_REFRESH_FULL`, and erases it 2.5 s later by
re-rendering chrome and blitting that rectangle back, also with FULL. Nothing
on the host can read a panel back, so whether the erase leaves residue where
the text was is a judgement that needs eyes. A FULL refresh of the same rect
should leave none -- and "should not" is what the DU4 folklore was made of,
which is the entire reason this line survives. #102 is the same gap from the
other side: even the plaque's geometry is unasserted.

## Per-system tuning nobody has measured

### 73. Six systems have a measured scale ceiling; nine have none

The mechanism exists (`ceiling` in `g_core_by_ext`, applied in
`config_resolve_profile_par`; in `KOBOY_LAYOUT_LCD` it caps the fractional fit
at N times the source instead of an integer scale) and six systems carry a
MEASURED number:

| ext | system | ceiling | what it was measured against |
|---|---|---|---|
| `.sfc` `.smc` | SNES | 3 | Star Fox 67% -> 79%, Kirby Super Star 78% -> 95% |
| `.sms` | Master System | 3 | Sonic Chaos 1172x768 83% -> 879x576 98% |
| `.gg` | Game Gear | 5 | Sonic Chaos 1152x864 79% -> 960x720 |
| `.md` | Mega Drive | 3 | Sonic 1264x966 -> 879x672 |
| `.min` | Pokemon Mini | 8 | the auto-13 sweep, 1248x832 22.3 ms -> 768x512 11.9 ms |
| `.gba` | Game Boy Advance | 4 | uncapped costs 40.7 ms of pipeline against a 16.7 ms frame |

**The remaining nine have none, and that is a statement about what has been
measured, not about what is safe.** `.mgw` `.nes` `.ws` `.wsc` `.ngp` `.ngc`
`.a26` `.col` `.int` `.pce` `.zip` all auto-fit uncapped. `tests/test_config.c`
lists them explicitly, so adding a ceiling to one has to come there and say so.
Which nine, and which two to look at first, is #78.

**The Game Gear is the cautionary tale and it should be read before adding
any row here.** Its number was right, unwritten, for months: 160x144
auto-fitted to exactly the Game Boy's 800x720 and TESTED.md said so. Then
`pixel_aspect` widened its rect to 192 columns, the auto-fit went 5 -> 6, and
the picture silently became 1.73x the Game Boy's on the same frame. A
measured number that lives only in a comment is not protected.

**Do not reach for `present_divisor` first when a system feels slow.** Measured
on Super Mario World at the full base-sized rect: divisor 3 gives 15,535 ms and
divisor 6 gives 15,354 ms -- a 1% difference, because the presentation count is
already content-bound (183 presented frames out of 900 at divisor 3, not the
300 the divisor alone would give). Rect area is the term that moves, and it
moves linearly.

### 78. Nine systems still auto-fit uncapped, and two of them look like the ones that just needed capping

Split out of #73 rather than left inside it, because #73's mechanism half is
now closed and this is the part that is not. Intellivision reaches 1408x896
on a 1440x1920 panel and ColecoVision 1024x768 on the verified one --- both
Master-System territory, and the Master System needed a ceiling. Neither has
been run at 900 frames on the device.

Do not guess a table. The Game Gear's number was wrong for months precisely
because it was believed rather than measured, and the Master System's turned
out to be 3 while the Game Gear's is 5 even though they are the same core.

### 79. The Mega Drive's ceiling was chosen from one title, in the session's noisiest window

`.md` is capped at 3 on the strength of Sonic alone, and its sweep ran late in
a long benchmarking session: its absolute figures are inflated relative to the
Master System and Game Gear sweeps taken an hour earlier (the same drift
TESTED.md's rect-sizing section documents). The ORDERING is unambiguous and
the choice follows from it, but the margin between 3 and 4 was measured at
74.6% against 90.4% under load, not at rest.

Virtua Racing --- the heaviest Mega Drive title this project has met, 84% at
957x720 before any of this --- is not on the device and was not measured at
all. If any title is going to want a ceiling of 2, it is that one.

### 87. Metroid Fusion sits ON the budget at the device's current divisor

Split out of #81 as the part that did not close. Measured with `--walk`, so
the screen is actually scrolling --- which is what `--mash` never makes it do
and why this number is 2.5x the one that run reported:

| | mean | p95 | budget at divisor 2 | speed |
|---|---|---|---|---|
| Metroid Fusion | 4,467 us | 5,236 us | 4,316 us | **99.1% / 94.8%** |

Every other title in both runs is inside, most at half the budget or less. So
this is one title, and the remedy is already a menu entry: at the SHIPPED
`present_divisor = 3` the same rect gives 8,458 us and Fusion sits at 53%.
The owner's device is on 2.

What would settle whether it matters is a playtest. 99.1% of full speed is
below the threshold anyone can see; the question is whether the p95 frames
CLUSTER (a visible stutter on entering a room) or scatter. A mean cannot
answer that and neither can `--walk`.

### 86. `--mash` is a blunt instrument and its blindness is asymmetric

`corebench --mash` holds START then A on a 32-frame cycle. That got seven of
the eight benchmark titles into real gameplay --- confirmed by rendering
frame 3000 of each through koboy's own pipeline and looking at it, which is
the only check that could confirm it --- and it failed on exactly the class
this system was added for: Pokemon, where START restarts the intro.

Two consequences worth separating. The measurement one is small: seven of
eight is enough for a mean. The one that matters is that **a masher measures
menus and text boxes, not combat**: it never presses a direction, so no
scrolling happens, and scrolling is both the expensive case for the core and
the bad case for the panel. The action-title figures in TESTED.md are
therefore the optimistic end of their range, and should be read that way
until a save state taken at a real gameplay position replaces them.

A `--keys` option taking a small script (`120:right`, `300:a+right`) would
fix both, and would be worth more than any other change to that tool.

### 56. 226 of the 227 arcade boards' device figures are still EXTRAPOLATED

`TESTED.md`'s arcade table quotes device figures derived from a host-to-device
ratio measured on two cores koboy had already run on hardware: gambatte's
`core` stage is 0.316 ms on this host against 2.3 ms on the Libra 2 (7.3x),
and fceumm's is 0.72 ms against 4.3-4.6 ms (6.0-6.4x). Arcade numbers are the
host figure times 7.

**One board has since been measured and the model held.** Galaga: host 0.68
ms, extrapolated ~4.7 ms, measured on the device at **4.4 ms** (`TESTED.md`,
"All fourteen systems run on the device") -- within 7%, which is better than
this entry expected. That is one data point at the *cheap* end of the range,
and the reason the entry stays open is the other end: the boards near the top
(Tapper at an estimated 12.7 ms) are close enough to a 16.7 ms frame that the
sign of the error decides whether they are playable, and a ratio validated on
a 4 ms board says little about a 13 ms one. Measure Tapper before believing
its row.

### 29. `present_divisor` may want to be per-core

G&W has **no scrolling**, so #25's smearing cannot occur and #26's
divisor/smearing tradeoff has nothing to trade. The shipped `3` was chosen
against a scrolling platformer; for a segment-LCD game where only a handful
of tiles change per frame it is probably too conservative. This is a config
question, not a code one, until someone measures it on the panel.

Do NOT reason about this from a G&W-vs-Game-Boy pixel-count comparison
against 160x144 -- see the correction in #30.

### 32. `present_divisor` may want to be per-system, not per-config

Filling the panel roughly doubled `submit` (see TESTED.md's LCD table), and
Game & Watch has no scrolling, so #26's divisor-versus-smearing tradeoff --
measured on a scrolling platformer -- does not apply to it at all. The
shipped `3` was chosen against Darkwing Duck. A per-layout default would let
Game & Watch present more often without touching the Game Boy's tuning.
Config question, not a code one, until someone reports a title feeling slow.

### 75. `present_divisor` is one global key, and the menu now makes that visible

The FRAMES row writes `present_divisor` in `koboy.ini`, which is per-CONFIG
and not per-system. Cycle it while playing a Game & Watch title and the Game
Boy's pacing moves too. That was always true of the ini key; what changed is
that it is now trivially easy to do by accident, from inside a game, with no
indication that the setting is shared.

#29 and #32 already argue the value wants to be per-core or per-system, and
for good measured reasons: Game & Watch has no scrolling at all, so the
smearing #26 trades against cannot occur there and 3 is almost certainly too
conservative, while the NES and the Neo Geo Pocket are much heavier per frame.
This entry does not make that worse in behaviour -- it makes it worse in
discoverability, which is the kind of gap that surfaces as "I changed
something in one game and another game got choppy".

The cheap fix is a per-system section in the ini (which does not exist yet);
the cheaper one is a word in the row itself. Neither is worth doing before
anyone has decided what value they actually want -- see #26's open half.

## Controls a player cannot reach

### 42. Two WonderSwan buttons are still unreachable in portrait

`wswan_rotate_keymap = auto` remaps the retropad when a title is rotated.
The faceplate now covers the d-pad (Y cursor), START, SELECT, the two
X-cursor buttons that land on `JOYPAD_A`/`JOYPAD_B`, and -- via the new L1/R1
discs -- the console's own A and B on `JOYPAD_L`/`JOYPAD_R`. What is left out
is the X cursor's UP and RIGHT, which land on `JOYPAD_Y` and `JOYPAD_X`. The
DMG faceplate has no X or Y disc and `KOBOY_MAX_EXTRA_BTNS` is 2 because that
is what fits between the d-pad and the B button without moving anything.

No title measured so far needs them -- GunPey's portrait mode answers
`JOYPAD_A`, Klonoa's answers `JOYPAD_L` -- so this is filed rather than
fixed. If a report arrives, the options are a third and fourth slot (needing
somewhere to put them) or a MENU toggle that remaps the two drawn face discs.

### 49. Ten of the ColecoVision's twelve keypad keys are unreachable

The DMG faceplate has room for two extra discs and a ColecoVision controller
has a twelve-key keypad. `config_extra_buttons_for_rom` spends both slots on
keypad 1 and 2, because those are the two the console's own BIOS option
screen asks for and without keypad 1 a cartridge cannot be started at all.
Gearcoleco puts 3-8 on the shoulders and thumbsticks and 9/0 on an analog
axis; koboy answers `RETRO_DEVICE_JOYPAD` only, so those eight are gone.
`*` and `#` are reachable, but only because the core binds them to
`JOYPAD_START` / `JOYPAD_SELECT`, which means koboy's START and SELECT discs
are LYING about what they do on this system -- the faceplate's labels are
moulded into `chrome.c` and are not per-system.

The titles this actually costs are the ones with in-play menus (Fortune
Builder, the Super Action titles), not the ones with a start screen. Two ways
out if it ever matters: a per-system label for the START/SELECT pillows, or
the modal trick FreeIntv uses -- but Gearcoleco has no such mode, so koboy
would have to draw the keypad itself.

### 50. The Intellivision disc is 16-way and koboy offers 8

FreeIntv maps the four cardinals to the retropad d-pad and synthesises the
four diagonals from pairs, which is what koboy's touch pad already produces.
The remaining EIGHT intermediate positions of the real 16-direction disc are
offered only on the LEFT ANALOG STICK, and koboy has no analog source at all
(`src/core.c` answers `RETRO_DEVICE_JOYPAD` and `RETRO_DEVICE_POINTER`).
Titles that steer finely -- Astrosmash's ship, Auto Racing, Night Stalker --
are coarser here than on the hardware. Nothing is unplayable; nothing is
exact either. A touch d-pad that reported an ANGLE rather than a bitmask
could feed the analog axes, which is a real feature and not a small one.

### 107. The DMG pill is drawn one column narrower than its touch zone

`chrome.c`'s `box()` and `input.c`'s `in_rect()` disagree by one column on an
even width: the drawn pill covers `[cx-w/2, cx+w/2-1]`, exactly `w`; the hit
test covers `[cx-w/2, cx+w/2]`, `w+1`. Rows agree (both inclusive, `h+1`), so
`box()` is also internally asymmetric -- inclusive in rows, exact-width in
columns. START, SELECT and MENU are the affected controls.

**It is one pixel and it is filed, not fixed.** A touch zone deliberately a
shade more generous than the drawn control is a defensible choice on a
finger-driven panel; nothing anywhere records it either way, and the row/column
inconsistency is what makes it look accidental rather than chosen. The owner
has not ruled on it.

The category is the useful part, and it is bigger than the pixel: nothing
asserts *"a touch at the centre of the DRAWN A disc reports `KOBOY_BTN_A`"*.
`tests/test_input_touch.c` never calls `chrome_render`; `tests/test_chrome.c`
never hit-tests a rendered buffer, and says the split is deliberate. What both
sides assert is "a touch at the layout's A permille reports BTN_A" -- derived
INDEPENDENTLY on each side, so it cannot catch the two drifting apart. Do not
"fix" the pixel before that invariant exists, because until then there is no
way to see whether the fix was right.

The d-pad is the larger instance of the same split and IS deliberate:
`chrome.c` draws a plus, `input.c` claims a full circle, so the diagonal
quadrants are live and undrawn. `chrome.h` acknowledges the one-row frame
difference and not the diagonals.

### 62. Six-button boards lose their shoulder buttons, and Defender loses Reverse

Counted across all 227 romsets: 45 boards bind JOYPAD_L, 48 bind R, 45/46
bind L2/R2, 26/14 bind L3/R3. The DMG faceplate has room for the two discs
this batch added (Y and X, buttons 3 and 4) and no more -- see
`KOBOY_MAX_EXTRA_BTNS`, #45. Every affected board is outside the pre-1990
scope except **Defender**, whose "Reverse" is on JOYPAD_R and is therefore
unreachable; Hyperspace and Thrust are reachable through the new discs.

### 45. `KOBOY_MAX_EXTRA_BTNS` is 2 for a spatial reason, not a design one

`koboy_layout.extra[]` is sized 2 because the DMG faceplate has room for
exactly two more discs -- the pocket below A (the Pokemon Mini's C) and the
column between the d-pad and B (the WonderSwan's L1/R1 pair) -- without
moving a Game Boy control or pushing `chrome_controls_top` up into the game
rect. A seventh system needing three would need somewhere to put the third,
not just a bigger number. Recorded so nobody raises the constant and
discovers that at render time.

## Saves, save states, and staying alive

### 84. One file in the collection crashes the process, and no floor can stop it

`4 Homebrew/Battlenetwork Rockman Crystal (PD).gba`, 4,194,304 bytes, valid
Nintendo logo, header naming "RockMan". gpSP takes SIGSEGV in `execute_arm()`
called from `retro_run()` --- so unlike the 212-byte `.smc` that produced
`config_min_rom_bytes`, this is **not** a short file and the fault is **not**
in `retro_load_game`. The load-site floor cannot see it and a size check
cannot describe it.

1692 of the owner's 1693 files load and run, so this is one file. But it is
the second core measured to do worse than refuse, and koboy has no general
answer to a core that segfaults mid-`retro_run`: the process dies, on a
device whose whole point is that it recovers to the browser.

mGBA runs the same file. vba-next crashes on it too. That is the shape of the
compatibility cost of choosing the fastest core, quantified rather than
asserted.

Two possible answers, neither cheap: run the core in a child process (a real
architecture change, and it would cover #72's twelve unswept cores as well),
or install a `SIGSEGV` handler that `longjmp`s back to the browser (fragile,
and the core's heap is then in an unknown state). Doing nothing is defensible
at one file in 1693; doing nothing SILENTLY is not, which is why this is
written down.

### 72. The SNES crash guard covers one core; nothing tells us which other cores do worse than refuse

snes9x2005 SIGFPEs on a `.sfc`/`.smc` under 8192 bytes instead of refusing it
(`% Memory.CalculatedSize` in `LoROMMap`, where `CalculatedSize` rounds down to
whole 8 KB blocks). `config_min_rom_bytes` now floors it at the load site.

What was NOT done is the same sweep for the other twelve cores. The sweep that
found this one was "load all 7,741 files in the collection and see what
happens", and it was run only for the three systems in this batch. A truncated
download or a `._*` stub is possible for every system koboy lists, and the
cheap version of this check is `corebench --frames 1` over a directory of
deliberately short files, one per extension. Anything that exits 136 or 139
rather than reporting a rejection needs a row in `config_min_rom_bytes`.

### 92. Two unexplained SIGSEGVs in the owner's own log

`koboy.log` holds three `rc=139` exits from 2026-08-27. One, at 14:20:34, is
the mid-session core switch, fixed in `2037722` and verified. **The other two
have never been explained**, and they are a different shape:

| time | what preceded it |
|---|---|
| 13:19:00 | eight `gray_map` cycles then `present_divisor` 4, 6, 8, in MODE_MENU -- no ROM switch anywhere in the session |
| 13:20:06 | the NEXT launch: Pokemon Emerald on gpSP at `present_divisor 8` (the menu had written 8 back to the ini), which logged every startup line and then faulted before presenting a frame -- no "stopped" line |

Both involve gpSP. Both sit next to `present_divisor = 8`. The pairing is
suggestive and it is not evidence.

**Attempted reproduction, on the device, and it failed:** six runs of Emerald
at `--frames 600`, three at divisor 8 and three at divisor 3, all `rc=0`. The
divisor alone does not do it. See #95 for what that failure to reproduce does
not rule out -- it is the more important half of this pair.

The device's live `koboy.ini` still says `present_divisor = 8`, so if the
divisor is involved at all, the owner's next launch is on the value that did
it. If a third crash of this shape appears, the first thing to try is the same
title at divisor 3.

### 95. What a `--frames` run cannot rule out about #92

Every run available to a remote session goes through `./koboy --frames N` with
**Nickel up, no takeover, no `EVIOCGRAB`, and no real touch input**. The
owner's crashes happened under `scripts/koboy.sh`, which stops Nickel, grabs
the input devices and drives everything from the panel. Running that over ssh
is forbidden (`docs/device-workflow.md`: it is the one mistake that has already
cost a reboot), **so the code path the crashes actually took cannot be
exercised remotely at all.**

This is a separate entry from #92 rather than a paragraph inside it because it
outlives the specific crashes: any future "not reproducible" verdict reached
from a remote session carries the same hole, and it is easy to read six clean
runs as evidence of absence when they never entered the suspect code.

So the honest state of #92 is: not the divisor by itself, not reproducible in
the mode a remote session can run, and the suspicious surface is the takeover
and input path rather than pacing. The next person with the device in hand
should try cycling `gray_map` repeatedly under a real takeover, which is what
preceded the first one.

### 61. Save states exist for arcade but some are 145 MB

`retro_serialize_size()` is non-zero on all 213 playable boards, so
`MODE_MENU`'s save states are the working way to keep an arcade game. The
range is 6 KB (Pooyan) to **151,911,260 bytes** (the DoDonPachi DaiFukkatsu
family). `src/state.c` allocates that in one block and writes it through
`safefile.c`; on a 512 MB device with three slots per ROM that is not going
to work, and nothing currently checks. The pre-1990 boards this batch is
scoped to are all under 120 KB, so this bites only if someone plays the
later hardware that happens to run.

### 44. Neo Geo Pocket saves do not go through sram.c, and nothing manages them

`retro_get_memory_size(RETRO_MEMORY_SAVE_RAM)` is 0 for every one of the
owner's ten `.ngp` titles on BOTH candidate cores, because an NGP cartridge
saves into flash. RACE writes that flash itself as `<rom>.ngf` in the
directory the frontend answers `GET_SAVE_DIRECTORY` with, and koboy answers
with `cfg.save_dir`, whose shipped default resolves to the install directory.
So saves should work -- verified on the host, where fourteen titles left
twelve `.ngf` files in an initially empty directory -- but:

- the file lands in `.adds/koboy/` beside the binary, not in a `saves/`
  subdirectory, and nothing in koboy names or manages it;
- `sram_save`'s atomic temp-file/fsync/rename discipline does not apply to
  it, because koboy is not the writer;
- an unwritable `save_dir` fails silently, exactly the way a working save
  looks.

`tests/test_core.c` now pins the callback's answer. What it cannot pin is
that the answer is a directory that exists and is writable, and the silent
failure is the part that makes this worth keeping: the three bullets above are
all still true, and none of them is visible to a player until a save is gone.

### 71. `core_sram`'s pin-at-load is load-bearing for exactly one core, and the PC Engine's save is not per-cartridge

Every previous system was mostly saveless --- 0 bytes of `RETRO_MEMORY_SAVE_RAM`
on all 82 Atari titles, all 28 ColecoVision, all 26 Intellivision, all 227
arcade boards. These two are the opposite. Measured with `corebench`, which
prints the save length at load and again after a warmup:

| core | at load | running |
|---|---|---|
| Genesis Plus GX (Sonic, Phantasy Star IV) | 65,536 | **0** |
| snes9x2005 (Zelda, Chrono Trigger, Metroid) | 8,192 | 8,192 |
| snes9x2005 (Super Mario World) | 2,048 | 2,048 |
| beetle-pce-fast (Military Madness) | 2,048 | 2,048 |

So `core_sram`'s pin-at-load (`1fb3802`) is **load-bearing for Mega Drive** ---
without it koboy would write a zero-length `.srm` over a real save --- and
merely harmless for the other two, whose lengths are constant and
per-cartridge. Recorded here rather than only in a commit message because the
pin looks like defensive over-engineering until you see the 65,536 -> 0 row,
and Genesis Plus GX is the only core in fifteen that does it. Nothing in the
test suite would catch its removal against the other fourteen.

One PC Engine oddity to know first: its battery RAM is the 2 KB "Backup Unit"
SHARED by every title that uses it, not a per-cartridge chip. koboy names the
`.srm` after the ROM like every other system, so each title gets its own copy
of what real hardware shared. Correct per title and free, but not what the
console did, and a reader will wonder.

### 90. A failed load holds the panel for 20 seconds unless someone taps

`platform_kobo_fatal` draws the message and then waits for a key or a touch,
bounded at 20 seconds so an unattended run cannot hang. That was written for
messages that preceded an exit; it now also runs on the way BACK to the MAIN
MENU, and a device session measured exactly 20 s of dead panel between the
failed ROM and the menu because nothing tapped.

With a user present this is right --- the message stays until they have read
it. Unattended it costs 20 s per failed ROM, which is why the host smoke
tests wrap these runs in `timeout 30`. If a future run ever needs to fail
several ROMs in a row, this wait wants an argument (or a shorter deadline for
the non-terminal case), not a deletion: an error nobody sees is the thing
this project keeps saying it will not ship.

## The browser, RECENT, and files on disk

### 89. `recent_prune_missing` deletes the history of an unmounted card

`access(path, F_OK)` fails for every ROM on an SD card that is not mounted,
so opening RECENT with the card out silently drops every row that lived on
it --- permanently, since the next successful pick writes the pruned list
back. This predates the load-failure work and was not introduced by it, but
the fix made it more visible: a stale row is now a recoverable, explained
event rather than a crash, so pruning is no longer the only thing standing
between the user and a dead row.

The Libra 2 has no card slot, so this cannot bite the one verified device.
The Clara, Elipsa and Sage entries in the panel table are a different story.
A fix would need to distinguish "the file is gone" from "the whole mount
point is gone" --- `stat()` the ROM's directory, or the mount root, before
concluding anything about the file.

### 39. `ROMLIST_NAME` is 128 and a real NES collection overflows it

Found by pointing the browser at the owner's actual `NES/` directory: one
file is skipped and reported as "1 entry not shown", because ROM names in a
translated NES set run long --
`Go Go! Nekketu Hockey Club - Multi-Sport Battle (Japan) [T-En by
Disconnected Translations v0.99] [Add by GAFF Translations v1.00] [n].nes`
is 138 characters. The cap behaves exactly as designed (a name that would
truncate is skipped, and the count is shown rather than swallowed), so this
is a size question, not a bug: 128 was chosen when "Tetris (World).gb" was
the shape of a filename. A dirent name is at most 255 bytes, and the row is
elided for display anyway, so 256 would cost 128 bytes per entry -- 2.5 MB
across a 20000-ROM hard cap, which is the real reason to think about it
rather than just raising it.

### 31. RECENT can show two identically-named rows

Folder navigation strips the folder prefix from a row's DISPLAY label. Paths
are unchanged and existing `recent.dat` rows are untouched, but two ROMs with
the same filename in different folders now render as the same row in RECENT.
Harmless until it happens; the fix is to show the folder on the row when a
duplicate label exists, not to put the prefix back on every row.

### 88. The RECENT list's `display` field is now vestigial, and the record cannot shrink without a version

`recent_name_from_path` derives a row's name from its path, and both
`recent_touch` and `recent_load` call it, so nothing reads a byte of the
stored `display` that the current build did not write. It is dead weight in a
6,720-byte file.

It stays because the record is FIXED-SIZE and that is the whole framing
scheme: `recent_load` asks `safefile_read_exact` for exactly
`KOBOY_RECENT_MAX * sizeof(entry)` bytes, so a length mismatch alone
identifies a corrupt or foreign file with no version byte anywhere. Dropping
the field changes the length, and every `recent.dat` already on a device
becomes "foreign" and loads as EMPTY --- ten entries of play history, gone,
to save 1,600 bytes.

The cheap version of this is to keep writing the derived name into the field
forever (what happens today) and never look at it. The real version needs a
framing change: a version byte, or a length that is allowed to vary. Nobody
needs it yet. Written down so that "why is this field here?" has an answer
other than "nobody noticed".

### 108. The empty-directory message still names three extensions out of twenty

`src/main.c`: `"no .gb, .gbc or .mgw files in\n%s"`. koboy has claimed twenty
extensions since that string was written, so a user whose `roms/` holds only
`.md` files is told, on a panel with no terminal, that there are no Game Boy
or Game & Watch files there -- which is true and is not the question they
asked. It is the wording `--message` exists to deliver, and `notify()`/
`fatal()` pick between two variants of it depending on whether a game has
already run.

Surfaced by `make lint`, indirectly: clang's `-Wformat-nonliteral` points at
the two call sites (the format string is a variable holding one of two
literals, which is fine), and reading them is what found the text. Not fixed
here because it sits in `main.c`, which the extraction work is about to move.

The fix is not "list twenty extensions" -- that is a sentence nobody reads.
"no games in\n%s" says the same thing and cannot go stale, and
`packaging/roms-README.txt` is where the list belongs and already is.

### 63. The `.zip` row will claim a zipped ROM for any other system

`config_core_for_rom` routes every `.zip` to FinalBurn Neo, which is correct
because nothing else koboy ships can open a zip at all -- but a user who
drops a zipped `.nes` into `roms/` now gets a browser row that fails to load
where before the file was invisible. The failure is diagnosable (the log
names the core, and FBNeo's own error page appears -- see #60) and the fix is
not a dat parser in a 40 KB front-end. If it is ever reported, the cheap
answer is a line in `roms/README.txt` saying koboy does not read zipped ROMs
for any system but arcade.

### 54. `.bin` is a legitimate ROM extension on three of these systems and koboy claims none of it

stella2014 accepts `a26|bin|mvc`, Gearcoleco `col|cv|bin|rom`, FreeIntv
`int|bin|rom`. koboy's allowlist claims only `.a26`, `.col` and `.int`, and
`tests/test_romlist.c` asserts that `exec.bin`, `grom.bin` and
`colecovision.rom` are NOT listed as games -- which is the whole reason. A
user whose Atari 2600 collection is named `*.bin` (a common TOSEC shape) will
see an empty browser and no explanation. If that is ever reported, the fix is
not to claim `.bin` -- it is to say so in `roms/README.txt`, or to add a
rename hint, because the two BIOS files this project now asks users to
install are literally `.bin`.

### 37. `.fds` is accepted by fceumm and not listed by koboy

fceumm's `valid_extensions` is `fds|nes|unf|unif`, and a real collection has
a `3 Famicom Disk System/` folder in it. koboy does not list `.fds`, because
the FDS needs `disksys.rom` in a system directory koboy has no concept of --
measured: an `.fds` fails `retro_load_game` outright with an empty system
directory. Supporting it means giving koboy a system directory, which is a
design decision, not an extension entry.

### 36. `[BIOS] ....min` is listed in the browser as a selectable game

The Pokemon Mini core links its own free BIOS and needs no dumped one
(verified against an empty system directory), so the `[BIOS] Nintendo Pokemon
Mini (World).min` that ships in a normal collection is inert -- but it is a
`.min`, so `romlist_is_rom` lists it. Deliberately not filtered: that
predicate is an allowlist of EXTENSIONS, and a name-prefix rule would also
hide a homebrew named that way and would be a second, invisible rule for a
user to discover when their file vanished. Revisit only if someone actually
selects it and is confused by what happens.

### 70. `.sgx`, `.chd`, `.bin` and `.gen` are refused, and each will eventually be asked about

Recorded so the next person answers from the decision rather than re-deriving
it. `.bin`/`.gen` for Mega Drive: the owner ruled `.md` only, and the file
counts back it (`.bin` belongs to 723 TI-99/4A files and eight other systems
before it belongs to 36 Mega Drive ones, plus the two Intellivision BIOS
files). 36 of their 1777 Mega Drive files do not list. `.sgx`: 7 files,
beetle-pce-fast implements neither the SuperGrafx's second VDC nor its
priority mixer, so it would draw one WRONG rather than refuse --- running
them needs `beetle-supergrafx` or the full `beetle-pce`, i.e. a second core
and a CPU bill, for 7 titles. `.chd`: 48 files, needs a system-card BIOS
nobody ships plus CD emulation and a different save path. None of these is a
bug report; if one is reopened it should be reopened as a decision.

## Coverage gaps: no automated test exists for these

### 105. `TEST_MAIN` makes every test body invisible to `gcov`

`make coverage` (added 2026-08-28) gives a real per-file number for `src/` and
**nothing at all for `tests/`**, and the cause is one macro. `tests/test.h`:

```c
#define TEST_MAIN(...) int main(void) { __VA_ARGS__; ... }
```

Every test file is `TEST_MAIN({ ...600 lines... })`, so gcc attributes the
whole body to the macro's expansion point. Measured on
`tests/test_video_aspect.c`: gcov reports **ten** instrumented lines for a
663-line file, the last of them `4636*: 53: TEST_MAIN({` -- one count for the
entire test.

**Verified, not inferred.** The same file with `TEST_MAIN({` hand-expanded to
`int main(void) {` gives full per-line and per-branch data: the 1200-iteration
sweep at line 630 reports 1200, and `misses++` at 639 reports `#####`.

Why it matters: the architecture review's whole argument for a coverage target
was that it "mechanically finds the zero-iteration loop class" -- the shape of
`tests/test_video_aspect.c`'s sweep and `tests/test_text.c`'s glyph
comparison. **It cannot, as the harness is written**, and both of those were
found by a human reading code instead. Coverage of `src/` is unaffected and
correct; this is only about seeing inside a test.

The fix is a `TEST_BEGIN` / `TEST_END` pair instead of one variadic macro,
which is a mechanical edit of all 28 test files and one header -- cheap, but a
28-file diff, so it wants its own commit and not a corner of a tooling one.
Note the second prize: branch coverage inside tests would also show the
`if (a2 == UI_SELECT)`-gated assertion class directly.

### 106. `config_profile_presentation_same` is at 0%, measured

The review predicted it (§2.8) and `make coverage` confirms it: `src/config.c`
lines 1594-1603, the whole function, are never executed by any of the 28 test
binaries. Its callers are `main.c` (not linked into any test binary) and two
*comments* in `tests/smoke_host.sh`.

What is unpinned is not the comparison but the deliberate OMISSION of
`base_w`/`base_h` from it, argued at length in `config.h`. Adding those two
fields would reintroduce the Game & Watch full-repaint-at-video-rate
regression and nothing anywhere would go red. It is a pure function of two
structs; a test is a dozen lines and needs no fixture.

Nine of the twelve uncovered lines in `config.c` are this function. The other
three are `fclose`/`rename` failure paths in the ini writer.

### 18. `MENU -> CHOOSE ROM` and `MENU -> QUIT` are driven by nothing

The `menu` verb closed most of this (#47): the emulator loop accepts scripted
input, and GREYSCALE, FRAMES, SAVE STATE and LOAD STATE are driven end to end
by `tests/smoke_host.sh`. **CHOOSE ROM and QUIT are reachable through exactly
the same hook and no test uses it on them.** They are the two rows that leave
the emulator loop, which is why they were the awkward ones to script and also
why they are the ones worth scripting: CHOOSE ROM is the path a mid-session
core switch takes (#93, and the crash in `2037722` that lived there), and QUIT
is the only clean exit a player has.

The owner has pressed both by hand on the device. That is not a regression
test, and this entry is filed under coverage for that reason.

### 5. A ROM that is deleted or unreadable has no test; a truncated one now does

`src/core.c`'s load failure paths. The truncated case is covered end to end --
`tests/smoke_host.sh` breaks a `.sfc` down to 212 bytes between two runs, picks
it from RECENT, and asserts recovery back to the menu and a second game
starting. The two siblings are not: nothing deletes the ROM between the scan
and the load, and nothing denies read permission on it. Both are user-reachable
(a card pulled mid-session, a file copied with the wrong mode), and the
truncated case is the one that proved the panel message and the recovery work,
so the remaining two are cheap to add against a path that already exists.

### 93. The video and input rebuild across a switch is a construction argument, not a test

`koboy_video` and `koboy_input` are locals of the session loop -- created
after the re-fit, destroyed at the loop's bottom, referenced nowhere else --
so no variable can carry one across a switch. That is a strong argument and
it is not a check: a mutant that deliberately keeps the video alive across a
switch produces IDENTICAL output on the host harness, because both stub cores
report 160x144 and only the layout differs, so nothing is dropped and nothing
is malformed enough to see from outside the process.

Making it observable needs two stub cores with DIFFERENT geometry in one
process. `KOBOY_STUB_GEOM` is one environment variable read by one stub, so
it cannot express that today. The cheapest fix is probably a stub that keys
its reported geometry off the ROM's extension, which would also let the
geometry re-fit be tested across a switch rather than only at startup.

### 102. The confirmation plaque's pixels are never asserted

`shot_note_rect` and the drawing beside it live in `main.c`, which is not
linked into any test binary, and no host backend can read the panel back. The
smoke test proves the plaque is NOT in the capture (zero pure-white pixels
outside the game rect, and the faceplate has none of its own) and that the
run does not crash with it enabled. It does not prove the plaque is legible,
correctly placed, or ever erased.

Moving the geometry into `shot.c` beside `shot_compose` would make the rect
testable; the drawing and the erase would still not be. The cheap version is
to assert the rect, which is where the clamps are.

### 103. One capture per `--ui-script` run, and that is the script's fault

The emulator loop's `menu` marker scan steps over idle states looking for the
next marker, so two `menu` verbs separated only by `idle` are consumed on
consecutive iterations with no frame presented between them -- both arms
collapse into one capture. A script therefore cannot drive two shots in one
process, which is why the numbering test uses two processes (the more
valuable run anyway: it is the one that proves the counter comes off the
directory).

What that leaves untested is a second capture taken while the first one's
plaque is still on the panel. It cannot happen -- every route to a second
shot goes through the MENU, whose close path repaints chrome over the plaque
and clears the timer -- but that is an argument, not a test. A `frames N`
verb that let a script run the emulator loop for N core frames would close
this and would be useful for more than screenshots.

### 19. The d-pad horizontal-arm term in `chrome_controls_top` is provably dead

`src/chrome.c:41-42`. `top = min2(top, dcy - arm/2 - 1)` can never
win against the line immediately before it (`dcy - dr - 1`): with
`arm = dr/3`, `dr > arm/2` for every `dr > 0` this layout ever produces,
so the vertical-arm term is always the smaller of the two. The equality
check in `tests/test_chrome.c` does not catch this, because the
function's return value is identical whether the line is there or not --
only a term-by-term audit finds it.

## Decisions on record, so nobody re-derives them

### 109. What `make lint` reports with its optional flags on, and why they are off

Three classes are real, none is a defect, and each is here so the next person
to widen `LINTFLAGS` does not re-derive them:

- **80 `-Wshadow` warnings, every one in `tests/`.** `src/` is clean on all
  three lint groups. They are the `int i` reused in a nested block that a
  600-line test body is made of. `-Wshadow` is therefore ON for `src/` and OFF
  for `tests/`; `make lint LINT_TEST_EXTRA=-Wshadow` shows them. Fixing them
  is 80 renames in six files for no defect, and it would collide with whatever
  #105 does to those files.
- **4 `-Wmissing-format-attribute` on `main.c`'s `say`, `message_v`, `notify`
  and `fatal`.** They are printf wrappers with no `format(printf, n, m)`
  attribute, so no call site's format string is checked. Adding the four
  attributes was TRIED on a scratch copy and both gcc and clang then report
  **zero** new warnings across the host and Kobo builds -- so it is insurance
  against a future call site, not a fix for a present bug, which is why it did
  not go into a tooling commit. Worth doing when `main.c` is next opened.
- **1 `-Wformat-truncation` on `src/probe.c`, from gcc and not clang.**
  `snprintf(path, sizeof path, "/proc/%s/comm", de->d_name)` into `char[64]`,
  where `d_name` can be 256. It is a false positive -- the loop has already
  refused any entry whose name does not start with a digit, and a PID is at
  most ten characters -- and it needs `-O1` or better, so `make lint`
  (`-fsyntax-only`) never sees it and `make kobo` has always printed it.

### 46. The shipped `gray_map` default was chosen on a host monitor, and the owner's device disagrees with it

Every frame that decided `gray_map = balanced` was rendered through the real
`video.c` and looked at **on a backlit sRGB display**. An e-ink panel's four
levels are not 0x00/0x55/0xAA/0xFF as reflectance -- the spacing is the
controller's and the white point is paper -- which is why the in-game MENU
entry exists at all.

**The owner's device runs `bright`, not the shipped `balanced`**, and nobody
wrote down why or whether it is the better default. On SMB's palette the two
differ by 10 grey levels on the sky and land on the same panel level (#96), so
the preference is about something else -- and since 1-bit output shipped (#25),
`gray_map` decides the dither's input rather than the visible level, which may
have changed the answer again. `koboy.log` names the active mapping on every
launch, so whatever a session settles on is recoverable; nothing has settled
it.

### 48. `value` can make a HUD disappear, and the menu offers it anyway

`gray_map = value` (`max(R,G,B)`) puts Super Mario Bros.' white HUD text on a
white sky: the text is gone, not merely low-contrast. It is shipped as a menu
option regardless, because it is genuinely the best of the five for line art
and text-heavy screens and the owner is entitled to see it -- but if a
"cannot render nothing" rule is ever wanted, this is the entry that breaks it.
`koboy.ini`'s comment says so in as many words. Recorded so the day someone
reports "the score vanished", the answer is one line away.

### 52. An Intellivision frame is mostly black border, on a reflective panel

FreeIntv's output is a fixed 352x224 with the 320x192 playfield inside it;
the rest is the console's border, which most titles leave black. On a Libra 2
that is a solid black band around a 1056x672 game rect -- both hard to read
on paper and the worst case for the panel's waveforms, which is the same
reasoning that already picked the Pokemon Mini's inverted palette
(`src/core.c`). The core has no option to trim it, so the fix would be
koboy's: either crop the known border on submit, or grow the existing
"unused corner is paper" idea into "a border the core draws every frame is
not content". Neither is obviously right, which is why it is written down
rather than done.

### 58. Galaga's starfield is a full-screen animation, and the "single-screen boards do not smear" premise is wrong for it

Measured with koboy's own dirty-rect pipeline, mean fraction of the game rect
changing per presented frame during real gameplay:

| Board | dirty per frame | why |
|---|---|---|
| Dig Dug | 1.5% | static earth, a few sprites |
| Donkey Kong | 1.9% | static girders |
| Ms. Pac-Man | 2.6% | static maze |
| Joust | 0.2% | static platforms |
| Frogger | 52.5% | every lane of logs and cars moves |
| Galaga | 67.1% | **the starfield scrolls continuously** |
| Galaxian | 85.5% | same, and denser |
| Xevious | 59.5% | scrolling shooter, as expected |

Galaga and Galaxian are single-screen games with a full-screen scrolling
BACKGROUND, so they smear like a scrolling platformer even though nothing
about the playfield scrolls. Recorded because the "arcade is single-screen, so
#25 does not apply" reasoning is wrong for exactly these two, and a reader
will make it again.

### 59. Arcade is the darkest content koboy has ever rendered

Mean luma of a rendered gameplay frame, after the four-level quantiser:
Galaga 0.013, Donkey Kong 0.093, Frogger 0.130, Ms. Pac-Man 0.163, Dig Dug
0.387. Galaga is **97.8% level 0**. The Pokemon Mini was called out in
`src/core.c` for being 83% black at mean luma 0.174 and got an inverted
palette for it; arcade is darker still and cannot take the same fix, because
light-on-dark IS the art -- inverting Ms. Pac-Man's maze would be wrong, not
better. Dig Dug shows the shape of the good case (a bright earth field), so
this is per-board rather than per-system. No action proposed; it is written
down because "arcade suits this panel" needs the qualifier.

### 60. FBNeo returns SUCCESS for a romset it cannot load, and draws an error screen instead

`retro_load_game` returns true for a missing or mismatched set, and the core
renders a 640x480 mostly-white page reading "This game is known but one of
your romsets is missing files for THIS VERSION of FBNeo". This is the
Gearcoleco `NO BIOS` situation exactly -- "it loaded" and "it works" are
different questions -- and koboy will show that page rather than a
`core rejected rom` line. It is arguably the better outcome (the page names
the problem) but it means koboy's own error path never fires for arcade, and
nothing in the test suite can tell a working board from a broken one.

The reliable discriminator, found by scanning all 227: an error page reports
base 640x480 with `retro_serialize_size() == 0`. Exactly 14 zips match, and
they are precisely the device/BIOS dumps a complete set carries (`neogeo`,
`midssio`, `namcoc69/70/75`, `nmk004`, `ym2608`, `cchip`, `pgm`, `skns`,
`isgsm`, `bubsys`, `decocass`) plus `wbmlb2`, whose parent `wbml.zip` the
owner does not have. One board, `astdelux`, legitimately reports 640x480 --
it is a vector game -- and has a real serialize size, which is why the
serialize half of the test is load-bearing.

### 53. `stella2014_libretro.so` is the largest ARM core shipped, and GPGX is larger

Genesis Plus GX cross-builds to 5.9 MB stripped, against gambatte's 2.6 MB
and RACE's 208 KB, because it carries a Mega Drive, a Sega CD (libchdr,
zstd, tremor, an MP3 decoder) and an SVP DSP that koboy will never load a
single ROM for -- `config_core_for_rom` routes only `.sms` and `.gg` there.
Nothing is broken and `dist/` is not size-constrained, but if it ever
becomes so, GPGX's Makefile has `HAVE_CDROM`/`USE_LIBCHDR` switches and the
right answer is to turn them off rather than to change core.

### 64. 7-Zip support is compiled out of the DEVICE core only

`scripts/build-fbneo-core.sh` passes `INCLUDE_7Z_SUPPORT=0` for the Kobo
target because `dep/libs/lib7z/CpuArch.c` does not compile against glibc
2.19's headers (`HWCAP_NEON undeclared`). The HOST target keeps it on, so the
two builds differ in a capability. No koboy code path reaches the difference
-- `.7z` is claimed by neither `config_core_for_rom` nor `romlist_is_rom`,
and `tests/test_romlist.c` asserts that -- but if `.7z` is ever wanted, the
device build is where the work is.

### 74. Arcade is in the DMG layout, and the LCD layout would give a vertical board 2.1x the picture

The arcade brief specified `KOBOY_LAYOUT_DMG` and the coordinator has since
called that their error. Evaluated, not shipped, and here is the whole trade.

**The gating question --- does the LCD strip cover an arcade board's controls
--- is the wrong question, because the DMG faceplate ALREADY does.** A board
needs a stick, up to four fire buttons, COIN and START. `config_extra_buttons_
for_rom`'s `.zip` case draws discs "3" (`JOYPAD_Y`) and "4" (`JOYPAD_X`)
beside A and B, and FBNeo binds Coin 1 to `JOYPAD_SELECT` and Start 1 to
`JOYPAD_START`, which are the faceplate's own pills. Nothing is unreachable
today. The LCD strip would add L1/R1, which only six-button boards want (#62).

**What LCD would buy, on the verified 1264x1680 panel:**

| Board | DMG (today) | LCD | area |
|---|---|---|---|
| Galaga (vertical) | 648x864 | 945x1260 | 2.13x |
| Defender | 960x720 | 1264x948 | 1.73x |
| Tapper | 640x480 | 1264x948 | 3.90x |

**What it would cost, and this is why it is filed rather than done:**

1. **Fractional scaling on pixel art.** The LCD fit is fractional by design
   (it exists so a Game & Watch unit fills the panel). Galaga at 945x1260 is
   4.22 columns and 4.375 rows per source pixel, so the checkerboard of an
   arcade sprite comes out with 4- and 5-wide cells beating against each
   other. Rendered both and looked: the DMG integer 3x is clean and the LCD
   fit visibly alias. This is the exact artifact `video_fit_par`'s
   integer-vertical rule was chosen to avoid.
2. ~~**The LCD rect is still sized from MAX, and FBNeo's max is SQUARE**~~
   --- **ANSWERED 2026-08-27**, by the task that moved SNES and Mega Drive
   into this layout. Which geometry the LCD rect comes from is now a
   PER-SYSTEM question (`config_lcd_rect_from_max_for_rom`): Game & Watch
   keeps max because its frame changes several times a second, and anything
   else takes base. So the objection below is no longer a reason not to move
   arcade --- it would take base like the two consoles do, and FBNeo's square
   max would never reach the rect.

   The original text, because the artifact it describes is real for anyone
   who reverts that flag: FBNeo's max is square (side = max(w,h), so both
   orientations fit one buffer), so a vertical board would get a 1216x1260
   rect holding a 945x1260 picture --- 135 px of permanent white band down
   each side, inside the recess.

   **Costs 1 and 3 below are untouched and are now the whole case.** The
   fractional-scaling artifact on arcade pixel art was rendered and looked
   at; one FBNeo board has been benchmarked on the device (Galaga, 4.4 ms core
   / 14.5 ms submit) and it is a cheap, small-rect one -- see #56.
3. **Cost.** 1.2 Mpx is SNES-at-scale-4 territory, and that is the size that
   cost Kirby 18 points of speed. The only FBNeo board measured on the device
   is Galaga, at its small DMG rect (#56), so there is no measurement of what
   FBNeo costs at 1.2 Mpx to weigh this against.

Recommendation: not without (a) an on-device FBNeo measurement and (b) an
answer to the square-max white bands. If the picture size alone is what
matters, #73's per-system scale is a cheaper lever on the same axis.

### 104. The PNGs are uncompressed, and one is 2.1 MB

`src/png.c` uses stored DEFLATE blocks, so a 1264x1680 capture is 2,125,257
bytes where a real encoder would produce perhaps 60 KB of it -- four greys
over large flat areas is close to the best case compression has. That is a
deliberate trade (no zlib, no invented compressor -- see the file's header)
and it costs disk space on a device with 24 GB of it.

It costs something else, though: uploading ten of these to a GitHub README is
21 MB, and the owner is on a laptop over wifi rather than the device. If that
becomes annoying, the answer is `optipng` on the desktop, not a compressor in
koboy -- and this entry exists so that trade is a decision rather than a
surprise.

---

## Retired numbers

Closed, fixed, removed, or answered by the owner playing on the device. Kept
as an index and nothing more, because source comments cite these numbers and a
citation that resolves to nothing costs a reader more than one line here does.
Where a closed entry held the only copy of a measurement, that measurement was
moved before the entry went; the destination is named.

| # | What happened to it |
|---|---|
| 1 | CLOSED. `tests/test_input_touch.c` (search `#1`) asserts the CROSS/RELATIVE distinction. |
| 2 | CLOSED. `tests/test_input_touch.c` (`#2`) passes `flip_x` and `flip_y` true. |
| 3 | CLOSED. Cartridge SRAM write, read and the destructive-truncation path all ran on the Libra 2 — `TESTED.md`, "The save path ran on hardware for the first time". |
| 4 | CLOSED. `force_dither` is the shipped default and runs end to end — `TESTED.md`, "1-bit output fixes the motion smearing". |
| 6 | CLOSED. `tests/test_config.c` now seeds a key the exact-strcmp filter actually matches. |
| 7 | CLOSED. `tests/test_core.c` reads the stub's observation flags through `dlsym`. |
| 8 | CLOSED. `as_bool` returns the default for an empty value (`src/config.c`). |
| 9 | CLOSED. An ini with no trailing newline no longer has the calibration block concatenated onto its last line; `tests/test_config.c` (`#9`). |
| 10 | CLOSED. `scripts/verify-core.sh` matches whole library names with `case`, not an unanchored `grep -E`. |
| 11 | CLOSED. `xdlsym`'s error names the `.so` path (`src/core.c`). |
| 12 | CLOSED. `bayer_ensure` carries the single-threaded-by-construction note (`src/video.c`). |
| 13 | CLOSED. `config/koboy.ini` states the install-relative rule once, above all three path keys. |
| 14 | CLOSED. `src/platform_kobo.h` exists. |
| 15 | CLOSED. `docs/probe-readme.md` names `make probe-dist`. |
| 16 | CLOSED. `video_scale_gray`'s preconditions are stated at the function. |
| 17 | FIXED. A scripted run is primed with one released state before the script's first entry, so a `tap`-first script works; a scripted run that selects nothing exits 4. |
| 20 | REMOVED, not fixed — the speaker grille is gone from the faceplate. The class of mistake it recorded (`hline`'s two-columns-per-call convention against an inclusive right edge) now lives as a comment at `hline` in `src/chrome.c`. |
| 28 | CLOSED. A Game & Watch title ran on the device. |
| 33 | CLOSED. NES and Pokemon Mini ran on the device. |
| 34 | SUPERSEDED. fceumm's per-frame cost is measured on the device (4.3-4.6 ms, `TESTED.md`), and the rect is no longer sized from a declared max, so the scale arithmetic in it described a build that no longer exists. NES is still one of the nine uncapped systems — that half is #78. |
| 35 | CLOSED. `.min` carries a measured ceiling of 8 and Pokemon Mini fills the panel; the sweep behind the number is in `src/config.c`'s `g_core_by_ext` comment. |
| 38 | CLOSED with #57. Frame time comes from each core's `retro_system_timing.fps`; `src/pacing.h`, `tests/test_pacing.c`. |
| 40 | ANSWERED. WonderSwan and Neo Geo Pocket have run on the device. |
| 43 | CLOSED. The greyscale mapping is selectable and the default is no longer Rec.601. Both measurements it carried — equal weights crushing *more* pixels to black than Rec.601, and the shadow lift taking that from 6.7% to 2.5% — are in `src/video.c`'s `GRAY_MAPS` and `KOBOY_GRAY_LIFT` comments. |
| 47 | CLOSED. `--ui-script` gained a `menu` verb and `tests/smoke_host.sh` drives GREYSCALE, FRAMES, SAVE STATE and LOAD STATE through it. What it still does not drive is #18. |
| 51 | CLOSED. The core's `geometry.aspect_ratio` is carried through the fit; `tests/test_video_aspect.c`, and `TESTED.md` has the shapes. |
| 55 | ANSWERED. Arcade has run on the device — Galaga, `TESTED.md`, "All fourteen systems run on the device". |
| 57 | CLOSED with #38. |
| 67 | CLOSED. The DMG rect is sized from the core's base geometry, not its declared max. |
| 68 | CLOSED. `corebench-arm` ran on the device; use `TESTED.md`'s measured per-title table rather than the two-point fit. |
| 69 | CLOSED. A PC Engine width switch resolves to a byte-identical presentation, so nothing is torn down and no diff history is lost. |
| 76 | ANSWERED. Save states have been written and re-read on the device. |
| 77 | ANSWERED. The six-button Mega Drive pad has been pressed on the device. |
| 80 | ANSWERED. The LCD control strip has been used by a finger on the device. |
| 81 | CLOSED. GBA is measured on the device and playable; `TESTED.md`. What was left of it is #87. |
| 82 | CLOSED. The kernel grants `PROT_EXEC` and gpSP's JIT cache is real — `TESTED.md`, "THE DYNAREC RUNS ON THIS KERNEL", which also carries the recipe and the mGBA fallback for a Kobo whose kernel does not. |
| 83 | ANSWERED. Cartridge saves have been made and reloaded by hand on the device. The device-side `.srm` round trips are in `TESTED.md`. |
| 85 | CLOSED. The GBA ceiling of 4 was measured and uncapped is impossible; `TESTED.md`, "The ceiling of 4, MEASURED", which now also carries the caveat this entry was kept open for. |
| 91 | ANSWERED. A failed load has been hit on the device by hand. |
| 94 | ANSWERED. The owner switches systems by hand and has not reported a stall. No one has put a number on it; if that is ever wanted it is a fresh entry, not this one. |
| 97 | CLOSED. DU on the Libra 2 is 144.4 ms + 15.8 ns/px, i.e. 153.5 ms at the shipped rect, and AUTO measures identical to it on 1-bit content — `TESTED.md`, "The panel's refresh cost, measured at last". |
