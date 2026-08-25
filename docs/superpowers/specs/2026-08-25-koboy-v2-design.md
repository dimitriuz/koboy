# koboy v2 — browser, menu, save states, faceplate, Bluetooth

**Status:** design approved, pre-implementation
**Date:** 2026-08-25
**Supersedes:** nothing. Extends `2026-08-24-koboy-design.md`, whose appendices
remain authoritative for everything v1 measured.

## 1. What v2 is

v1 is merged and verified on one Kobo Libra 2: a full game of Tetris and a full
run of an action platformer, exiting to a working Nickel without a reboot. What
it is not is *finished*. You change games by editing an ini file over USB, the
faceplate's four controls are unlabelled grey shapes, the save path has never
executed on hardware, and there is no way out of a running game but the power
button.

v2 addresses exactly that, in five threads:

| Thread | Contents |
|---|---|
| **Player UI** | ROM browser at launch, an in-game menu, save states |
| **Faceplate** | A DMG-faithful procedural chrome with labels, bezel and a battery lamp |
| **Video** | Multi-rect dirty regions, decided by a measured cost model |
| **Bluetooth** | Game controllers, and audio — see §8, and Appendix A |
| **Closure** | The 16 findings in `docs/FOLLOWUPS.md`, folded into the tasks that touch them |

### Explicitly not in v2

- **GBA.** Unchanged from v1: a ~1GHz A9 will not run mGBA.
- **Colour on Kaleido panels.** §9 of the v1 spec records this as an accepted
  future breaking change to `blit_gray8`. v2 does not make it, so the blit seam
  stays stable through all of the above.
- **Device breadth as a goal.** Nothing here is designed for hardware nobody
  has tried, beyond continuing to derive everything from runtime capability.
  `TESTED.md` still has exactly one verified row and v2 does not pretend
  otherwise.
- **Pairing Bluetooth devices from inside koboy.** See §8.4.

## 2. Architecture: the mode machine

`main.c` is 559 lines and already carries argument parsing, a bitmap font, the
first-run calibration UI, three hand-copied chrome restores, SRAM policy and the
emulator loop. Three new screens do not fit there, so v2 extracts a UI layer
first and everything else builds on it.

### New modules

| File | Purpose | Purity |
|---|---|---|
| `src/text.c/.h` | The 5x7 font — grown to digits and punctuation — plus `text_draw`, `text_draw_centred`, `text_measure` | pure |
| `src/ui.c/.h` | One list widget: title, items, paging, hit-testing, rendering | pure |
| `src/romlist.c/.h` | `readdir` scan for `.gb`/`.gbc`, sorted, capped | impure, isolated |
| `src/state.c/.h` | Save-state slots: path derivation, atomic write, whole-or-nothing load | impure, isolated |
| `src/platform_kobo.h` | The backend prototypes currently duplicated in two files (closes follow-up #14) | — |

The font moves out of `main.c` verbatim. It has to grow anyway: today's table is
A–Z and space, which renders a filename as `ZELDA  USA EUROPE  REV  GB`.

**The browser and the in-game menu are the same widget.** Both are a titled list
of strings you tap, with paging. `ui.c` is one implementation with two content
sources; writing them separately would be duplication wearing a disguise.

### Modes

```c
enum { MODE_BROWSE, MODE_PLAY, MODE_MENU, MODE_QUIT };
```

Two entry rules preserve every v1 behaviour:

- **An explicit `--rom` or `rom=` goes straight to `MODE_PLAY`.** Every existing
  smoke test, `--frames` run and scripted path is unchanged. The shipped
  `config/koboy.ini` leaves `rom` commented out, so a real user starts in
  `MODE_BROWSE`.
- **The `no rom configured` fatal survives**, but fires only when the browser
  also found nothing. "You have no ROMs" and "you mistyped a path" are different
  diagnoses, and on a device with no terminal that distinction is the whole
  diagnostic.

The core is **paused** in any UI mode — no `retro_run`, no presentation.

### Two small additions the mode machine forces

- **`video_invalidate(v)`.** After a UI mode the `prev` buffer is stale, so
  returning to `MODE_PLAY` must redraw the game rect whole. One `memset` to the
  same `0x01` impossible-value `video_create` already uses. Without it the first
  frame back diffs against a screen that is no longer on the panel, and the
  chrome-covered region silently keeps stale pixels.
- **A single `redraw_chrome()` helper**, replacing the three hand-copied restore
  blocks already in `main.c` (post-calibration, post-`fatal`, post-SRAM-warning).

`main.c` should end up **smaller** than it is now.

## 3. The list widget

`ui.c` renders into the caller's full-panel gray8 buffer and never calls the
platform, so it is golden-image testable exactly like `chrome.c`. `main.c` owns
the blit and the refresh.

The seam that makes it testable is that it consumes **state, not events**:

```c
typedef enum { UI_NONE, UI_SELECT, UI_PAGE_NEXT, UI_PAGE_PREV, UI_CANCEL } ui_action;
ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st, int *out_index);
```

`ui.c` holds the previous state and acts on **edges**: a tap is accepted on
touch-down and not accepted again until release. Level-triggered would select an
item sixty times a second. This is the same lesson as the d-pad's hysteresis in
a different costume, and it unit-tests against synthetic `koboy_input_state`
structs with no device and no golden image.

**Paging uses the page-turn buttons**, which is what they are named after. In a
UI mode `KOBOY_BTN_A`/`B` — already the calibrated hardware keys — mean previous
and next page rather than A and B. No new plumbing in `input.c`. A touch-only
Kobo pages by tapping drawn arrows instead.

Rendering uses `KOBOY_REFRESH_FULL`. A menu redraw is one GC16 flash of a panel
that is about to sit still; crisp 16-level text is worth ~400 ms there, and the
game rect's four-level ceiling does not apply to it.

## 4. The menu, and reaching it

### The MENU zone

Opening the menu needs a signal that is **not** a joypad bit. There is no
libretro button for "menu", and borrowing an unused bit would forward it to the
core. So the MENU zone gets its own latched, edge-triggered flag:

```c
bool input_take_menu_request(koboy_input *in);   /* true once per tap, then clears */
```

`koboy_layout` gains `menu_cx, menu_cy, menu_w, menu_h`. Computed against the
shipped defaults on the verified Libra 2 (1264x1680): the game rect ends at
y=804 and `chrome_controls_top` returns 1018, leaving a clear 214 px full-width
band. `menu_cy = 540‰, menu_h = 55‰` lands at y 830–984, clear of both.

**The MENU zone must be added to `chrome_controls_top`.** That function's
contract is "the topmost row any drawn control *or live touch zone* occupies",
and it exists because a `scale = 0` auto-fitted rect once swallowed the A button
while its touch zone stayed live underneath — tapping the lower playfield
pressed A. A new live zone the function does not know about reintroduces exactly
that bug on auto-fit panels. Its `min2` chain grows by one term per drawn
control, and §6 adds several.

### Menu structure

```
MENU  →  Save state  →  Slot 1 / 2 / 3
         Load state  →  Slot 1 / 2 / 3
         Reset game
         Choose ROM…      (back to MODE_BROWSE)
         Resume
         Quit
```

One level deep, using the same widget for both levels.

### Why not the power button

The power button quits cleanly today, and that is the guaranteed escape hatch. A
menu that fails to draw or hangs must never be able to trap the user on a device
where a stuck application is indistinguishable from a brick. Power keeps meaning
quit; the menu's Quit entry is a convenience, not the only exit.

## 5. Save states and the core lifecycle

`core.c` already binds `retro_unload_game` and `retro_reset`. Only the
serialisation trio is missing — v1 left it out deliberately. Four additions:

```c
bool   core_unload_rom(koboy_core *c);                     /* retro_unload_game  */
size_t core_state_size(koboy_core *c);                     /* 0 == unsupported   */
bool   core_state_save(koboy_core *c, void *buf, size_t n);
bool   core_state_load(koboy_core *c, const void *buf, size_t n);
```

The serialisation symbols bind **optionally**, not through the existing hard
`BIND` macro. A core lacking them must still play games; the menu greys the
entries out. The stub core in `tests/` is the immediate reason, but the general
rule is that a missing optional symbol is a capability answer, not a fatal error.

### Storage

Three slots at `<save_dir>/<stem>.st1..3`, listed with occupancy
(`Slot 2 — empty` / `Slot 2 — 14:22` via `stat`). Knowing which slot you are
about to overwrite is most of the value.

`state.c` mirrors `sram.c` on purpose, including the property that file's comment
records at length: reading straight into live memory and *then* reporting failure
destroyed the save it was loading. A save state loaded into a running core has
the identical failure shape and gets the identical defence — read whole into a
temp buffer, hand it to the core only if complete. The atomic writer
(temp file + `fsync` + `rename`) is factored out of `sram_save` so there is one
implementation rather than two that drift.

### Three lifecycle hazards, recorded so they are not discovered

- **`core_sram()` must be re-fetched after a ROM switch.** The pointer belongs to
  the core's freshly loaded cartridge. Caching it across `unload`/`load` is a
  use-after-free waiting for a second game.
- **SRAM is flushed *before* unload**, never after. `retro_unload_game` takes the
  buffer, and the outgoing game's last minutes with it.
- **Loading a state rewrites cartridge RAM**, since gambatte's serialised blob
  includes it, and the 10-second periodic flush then writes that to `.srm`. This
  is correct, but it means a state load is also indirectly a save-file write.

### Closing the SRAM gap

Both v1 titles report `rambanks: 0`, so `sram_load` and `sram_save` have never
executed on the device, and `sram_load` changed materially in v1's final fix
round. The verification ROM is
`Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2).gb`, whose header
reads cartridge type `0x03` (MBC1+RAM+BATTERY) and RAM size `0x02` (8 KiB, one
bank). `core_sram()` therefore returns a live 8192-byte pointer and the save path
runs for real. This closes follow-up #3 with a pass/fail rather than an
inspection.

## 6. A DMG-faithful faceplate

`chrome_render` currently draws a white background, a uniform 6 px black bezel, a
grey cross, two grey discs and two grey pills. Nothing is labelled: A, B, Start
and Select are four indistinguishable grey shapes.

**One resource nobody has spent:** the chrome is drawn with
`KOBOY_REFRESH_FULL`, which is GC16 — *sixteen* levels. The four-level ceiling
constrains the **game rect only**. The faceplate uses three values (`0xFF`,
`0xAA`, `0x00`) out of sixteen available. A real tonal ramp — case tone, recess
shadow, button fill, highlight — costs nothing at runtime, because v1's §5
already establishes that chrome elaborateness is an authoring question and not a
performance one.

| Element | DMG reference | Notes |
|---|---|---|
| Screen surround | Deep bezel, notably taller below the screen | Replaces the uniform 6 px frame. The asymmetry is what makes it read as a Game Boy |
| Strapline | `DOT MATRIX WITH STEREO SOUND` | Copying it verbatim was a false claim until §8. If Bluetooth audio ships, A2DP is stereo and the line is simply true. Decided at implementation time by whether audio landed |
| Power lamp | Red LED left of the screen labelled `BATTERY` | Drawn as a ringed disc plus label; live, see below |
| Button labels | `A` / `B` below the discs, `START` / `SELECT` below the pills | Exactly where the DMG puts them. Axis-aligned; the real ones are tilted, but rotated text is a great deal of machinery for very little |
| Speaker grille | Six diagonal slots, lower right | Pure decoration, pure geometry |
| Wordmark | Lower-left, where Nintendo's logotype sits | **`koboy`, not Nintendo's mark** |

### Trademark

This is a public GPLv3 repository. The faceplate is an homage to the *industrial
design* and carries **none of Nintendo's word marks or logotypes** — not
"Nintendo", not "GAME BOY". The wordmark position is filled with `koboy`.

### The battery lamp is live, and free

`platform_if` gains an optional `battery_percent()` returning -1 when unknown.
`platform_kobo.c` reads `/sys/class/power_supply`; `platform_sdl.c` returns -1
and the lamp draws static.

It is redrawn **only when the whole panel is already being repainted** — startup,
menu exit, a chrome restore. No timer, no new periodic refresh, so "chrome is
drawn once and never touched per frame" survives intact. The reading is slightly
stale and always free. A dedicated timer was rejected: on a panel where every
refresh is visible, adding a periodic one to display a number that changes over
hours is a bad trade, and it would give the ghosting-cleanup logic something new
to argue with.

### Invariants the redraw must not break

- **Never write inside the game rect** — `chrome_render`'s contract, and why
  `chrome_bands` exists with its clamps and its four-round history.
- **Every new element joins `chrome_controls_top`** — see §4.
- **Still drawn once.** Zero per-frame cost is what makes elaborate chrome
  affordable at all.
- **Still procedural, still permille.** A fixed bitmap would need authoring per
  device class and would undercut v1 §3.

## 7. Video: multi-rect dirty regions, measured first

### Measurement comes first

The loop gains per-stage accumulators — `core_run`, `video_submit`, `blit`,
`refresh` — reported at exit beside the existing `presented=` line, plus counts
of rects emitted and how often the split heuristic fires versus declines.
`platform_kobo_refresh_stats` already exists to extend.

This sequences **before** any tuning, and the reason is in this project's own
record: A2 was the obvious fast waveform and lost to DU4 by 3.5x; DU4 then lost
to AUTO. Twice, a settled decision was overturned by a number. Appendix B of the
v1 spec is also blunt that three readings of identical parameters spanned a
factor of 2.2, so a threshold derived from those figures would be false
precision.

The baseline run is Zelda, which also exercises §5's SRAM path.

### The split

`video_submit` returns one merged bounding box today. A sprite in the top-left
and a status bar in the bottom-right merge into a near-full-rect refresh whose
interior has not changed.

```c
int video_submit(koboy_video *v, const void *src, int w, int h, size_t pitch,
                 koboy_pixfmt fmt, koboy_rect *out, int max_out);   /* returns count */
```

Since refresh cost is `fixed + area`, the split is decided by an explicit model:

1. `video_dirty_rect` already walks an 8x8 tile grid — keep the walk, retain the
   per-tile dirty bits.
2. Segment into bands of consecutive dirty tile-rows separated by at least one
   clean row; split each band on columns the same way.
3. Emit the split **only if** `Σ cost(sub-rect) < cost(merged bbox)`, where
   `cost(r) = FIXED + area_in_tiles`.

`FIXED` ships as a config key in tile units rather than a compiled-in constant,
for the same reason `scale` and the waveform are config-overridable: the device
gets to correct us without a recompile.

### Honest limit

**This does nothing for a full-screen scroller.** Darkwing Duck's 13.1 fps is the
panel's answer, not the algorithm's, and no rectangle strategy improves it. The
win case is a mostly-static screen with two or three moving things — which is
most of Zelda, and every menu and dialogue box in every game.

### It is a correctness change, not only a speed one

A test must assert that the union of emitted rects covers **every** dirty tile,
on synthetic frames with separated changes. If the split ever drops a region the
panel silently keeps a stale pixel — the worst class of e-ink bug, because it is
indistinguishable from ghosting.

## 8. Bluetooth

v1 §1 deferred audio on the grounds that *"the target devices have no speaker;
Nickel owns the Bluetooth audio stack"*. **The second half of that is wrong**, and
Appendix A is the measurement that overturns it.

### 8.1 What the device actually has

Measured on the Libra 2, 2026-08-25, read-only with Nickel running. Full output
in Appendix A. The short version:

- BlueZ 5.x is present and **running**: `/libexec/bluetooth/bluetoothd`, owning
  `org.bluez` on the system bus, with the **`input` plugin** (`org.bluez.Input1`).
- `bluealsa` is present and **running**, bridging A2DP to ALSA, with
  `/etc/alsa/conf.d/20-bluealsa.conf` defining a `bluealsa` PCM.
- `aplay` and `amixer` are on the device.
- The kernel has `CONFIG_BT=y`, **`CONFIG_BT_HIDP=y`**, `CONFIG_UHID=y`,
  `CONFIG_HIDRAW=y`; `HIDP`, `L2CAP`, `RFCOMM`, `SCO` and `BNEP` are registered,
  and `/dev/uhid` exists.
- The radio is a Realtek attached over UART: `hci0`, `UP RUNNING`.
- Kobo's own `/etc/bluetooth/main.conf` sets
  `ReconnectUUIDs=00001124-…,0000110b-…` — **HID and A2DP Audio Sink** — with a
  ten-attempt backoff. BlueZ here is already configured to reconnect gamepads.

### 8.2 The decisive fact: what survives the takeover

| Process | PID | PPID | PGRP |
|---|---|---|---|
| `nickel` | 1973 | 1 | **1874** |
| `rtk_hciattach` | 3784 | 1973 | **1874** |
| `bluetoothd` | 3790 | **1** | 233 |
| `bluealsa` | 3786 | **1** | 233 |
| `dbus-daemon --system` | 233 | **1** | 233 |

`bluetoothd`, `bluealsa` and `dbus-daemon` are init children in their own process
group. **Stopping Nickel's process group does not touch them.**

`rtk_hciattach` is the interesting case: Nickel spawned it and it sits in
**Nickel's** process group, so a process-group kill would take it — and `hci0`
with it, since the UART line discipline is released when the attach process
dies.

**Correction, from reading `scripts/koboy.sh` rather than from the process
table.** The launcher does not kill by process group. It kills by name:

```sh
killall -q -TERM nickel hindenburg sickel fickel strickel fontickel \
                 adobehost foxitpdf iink
```

`rtk_hciattach` is not in that list, so the takeover leaves it running and it is
merely reparented to init. `hci0` should therefore survive as things stand. That
is a conclusion from reading the launcher, **not a measurement** — verifying it
requires an actual takeover, which is the one operation with a
device-corruption incident in its history, so it is verified on-device in the
Bluetooth plan rather than assumed here.

The prescribed behaviour is unchanged either way, and deliberately so:

**koboy owns bringing the radio up, by capability check rather than by
assumption.** `koboy.sh` checks for `hci0`
after stopping Nickel and, if absent, respawns the attach helper with the argv
observed here (`/sbin/rtk_hciattach -n -s 115200 ttymxc1 rtk_h5`), waiting for
`hci0` to appear. Checking rather than assuming covers both cases with one code
path: the helper surviving the takeover, and Bluetooth simply having been
switched off before launch — which is the common case and the one that would
otherwise leave a user wondering why their gamepad does nothing. Nickel's own strings name three variants — `RealtekHciAttach`,
`NXPHciAttach`, `CypressHciAttach` — so the helper is selected by what exists on
the device rather than hardcoded, in keeping with v1 §3.

This is also the case where Bluetooth was simply switched off before launch: no
`hci0`, no attach process, and the same code path brings it up.

### 8.3 How koboy uses the stack without linking to it

`CLAUDE.md`'s hard constraint is that the shipped ARM binary's dependency closure
stays exactly `libm.so.6` + `libc.so.6` + `ld-linux-armhf.so.3`, enforced by
`scripts/verify-core.sh`. The device carries `libbluetooth.so.3`,
`libasound.so.2`, `libdbus-1.so.3`, `libsbc.so.1` and `libglib-2.0.so.0` — and
koboy links **none** of them.

It does not need to. Every capability is reachable through processes that already
exist on the device:

| Need | Mechanism | koboy's own dependencies |
|---|---|---|
| Bring the radio up | `fork`/`exec` the attach helper | libc |
| Read a gamepad | `/dev/input/eventN`, which `input.c` already decodes | libc |
| Play audio | write PCM into `aplay -D bluealsa:DEV=…` over a pipe | libc |
| Find the audio device | parse `bluealsa-cli list-pcms` output | libc |

`fork`, `exec`, `pipe` and `write`. The closure rule is preserved by
construction, not by care.

### 8.4 Controllers

A paired HID device gets an evdev node from bluetoothd's `input` plugin. That is
the node type `input.c` was written for, and first-run calibration already learns
button mappings with no keycode table — the mechanism v1 §7 chose precisely so
that untested hardware works without code changes. A gamepad is, to koboy,
another key device.

Work required: extend node classification to adopt a device that appears
*after* startup, generalise calibration beyond two buttons (a gamepad has a real
d-pad, A, B, Start and Select), and add config keys for the mapping.

**This is the largest playability improvement available in v2.** v1 §7 calls the
touch d-pad *"the main threat to playability"* — no tactile feedback, on a panel
confirming input ~50 ms late. A physical gamepad deletes that problem rather than
mitigating it.

### 8.5 Audio

The path is `gambatte → S16 PCM → pipe → aplay -D bluealsa → A2DP`. Concretely:
`GET_AUDIO_VIDEO_ENABLE` starts reporting audio on, the
`audio_sample_batch` callback — which v1 already installs as a required
discard-everything stub — starts forwarding, and a bounded ring buffer feeds the
pipe with non-blocking writes that **drop** on overrun rather than stalling the
emulator loop.

**The path is exercised, not merely inspected.** With headphones connected, two
seconds of silence played successfully through
`aplay -D "bluealsa:DEV=<MAC>,PROFILE=a2dp"` (Appendix A). Two facts came out of
that which the design has to respect:

- **The PCM needs the device address explicitly.** Kobo's
  `/etc/alsa/conf.d/20-bluealsa.conf` sets `service`, `profile`, `delay` and
  `battery`, but never `defaults.bluealsa.device` — so a bare `-D bluealsa`
  fails with *"Unable to find definition 'defaults.bluealsa.device'"*. koboy
  discovers the address by running `bluealsa-cli list-pcms` and parsing the
  `dev_XX_XX_XX_XX_XX_XX` component of the `a2dpsrc/sink` path. Another pipe,
  still no linkage, still libc only.
- **The stream is opened once and held.** Two seconds of audio took 3.09 s wall
  clock, i.e. roughly **1.1 s of A2DP stream setup**. Opening per sound effect
  is impossible; koboy opens the PCM at startup and writes into it for the whole
  session. That is the natural shape for a streaming pipe anyway.

Three things remain unknown and must be measured before audio is called done:

1. **Latency.** A2DP plus `defaults.bluealsa.delay` plus codec buffering is
   plausibly 100–200 ms. On a panel already presenting at 13–25 fps the video is
   not tightly synchronised either, but audio that lags visibly is worse than no
   audio. Measure before shipping it on by default.
2. **CPU.** gambatte's sound emulation is real work on a single-core A9, and
   audio is currently switched off at the core. Presentation is the measured
   bottleneck, so there should be headroom — "should be" is not a measurement.
   The attempt to measure this on 2026-08-25 was invalidated by the wedge below
   and produced no usable number.
3. **Whether the PCM accepts gambatte's native 32768 Hz**, or whether koboy must
   resample to 44100. Untested for the same reason.

Audio therefore ships **off by default** (`audio = false`) until those numbers
exist, and the strapline in §6 follows the outcome.

### 8.5.1 A stuck client wedges bluealsa, and killing it is not enough

Measured accidentally but reproducibly on 2026-08-25. An `aplay` holding the
A2DP PCM was orphaned when its parent shell died. `bluealsa` then sat in
`futex_wait_queue_me` and stopped answering D-Bus entirely — `bluealsa-cli
list-pcms` timed out — while the ACL link and `bluetoothd` stayed perfectly
healthy. **`SIGKILL`ing the orphaned client did not recover it.** Only
restarting `bluealsa` did.

This is the same class of hazard as `EVIOCGRAB` in v1 §7: a resource koboy holds
that outlives a crash and degrades the device for everything after it. It gets
the same treatment.

- The audio child is spawned in **koboy's own process group**, and the restore
  path in `koboy.sh` kills it explicitly — in the trap, on every exit path,
  exactly like the framebuffer depth restore.
- koboy's signal handler closes the pipe and reaps the child, alongside the
  existing SRAM flush and grab release. Minimal work, no new hazards in a
  `SIGSEGV` path.
- Because even that is not sufficient — a hard crash can still orphan it — the
  launcher checks on **startup** for a stale `aplay` on the bluealsa PCM and
  clears it before opening its own. Recovering from the previous run's crash is
  cheaper than preventing every crash.
- If `bluealsa` is unresponsive at startup, audio is disabled for the session
  with a log line, and the game runs silently.

**Why this differs from `rtk_hciattach` in §8.2, since the two look contradictory:**
koboy respawns the attach helper because *koboy's own takeover killed it* — it
sits in Nickel's process group, and restoring what we broke is part of the
restore path's existing job. `bluealsa` is an init child in `dbus-daemon`'s
group that the takeover never touches, so a wedged `bluealsa` is a fault koboy
did not cause and cannot safely own. Repair what you broke; report what you
did not.

### 8.6 What koboy does not do: pairing

Pairing needs an agent to answer BlueZ's authorisation calls, and Nickel owns
`com.kobo.bluetooth.Agent` — which does die with the takeover. Registering our
own agent means speaking D-Bus, and the only ways to do that are linking libdbus
(forbidden) or driving `bluetoothctl` over a pipe (possible, fiddly, and a
sizeable subsystem for a once-per-device action).

So: **koboy never pairs.** Devices are paired in Nickel's own settings, or over
SSH with `bluetoothctl`, and koboy relies on BlueZ's existing `ReconnectUUIDs`
policy — which Kobo already configures for HID and A2DP — to reconnect them.
koboy brings the radio up, waits, and uses whatever appears.

This is the smallest design that works, it costs nothing in the closure, and
pairing is a once-per-controller action that nobody performs mid-game.

## 9. Follow-up closure

Every item in `docs/FOLLOWUPS.md` lands inside a task that already opens that
file. None gets its own phase.

| Task | Closes |
|---|---|
| Measurement + Zelda baseline | **#3** SRAM on hardware |
| `main.c` restructure | **#14** `platform_kobo.h` |
| ROM browser | **#5** ROM failure paths |
| MENU zone: layout, chrome, input | **#1** cross d-pad, **#2** `flip_x`/`flip_y`, **#6** ini-preservation filter, **#8** `as_bool`, **#9** trailing newline, **#13** ini comments |
| Save states / `core.c` | **#7** stub-core observation flags, **#11** `xdlsym` error path |
| Multi-rect video | **#4** `force_dither` end-to-end, **#12** `g_bayer` threading note, **#16** `video_scale_gray` preconditions |
| Packaging and docs | **#10** `verify-core.sh` anchors, **#15** `make probe-dist` |

**#5 stops being theoretical the moment the browser ships.** Today a bad ROM path
is a config typo; with a file list you can tap, choosing a truncated download or
a `.gb` that is not one becomes a routine user action, and `core.c`'s failure
paths move onto the main path.

## 10. Testing

`CLAUDE.md` records that three v1 tests passed whether or not the code they
guarded was present, one taking four review rounds to fix. So: **every safety or
regression test written here has its guard broken, and the mutant and its output
recorded.** A test that can only fail via undefined behaviour is not a test.

### The trap this design walks into

v1's endgame recorded that the first-run deadlock was invisible to twenty
per-task reviews because *the scripted-run branch skips calibration — every
automated test took the one path that could not reach the bug.*

§2's compatibility rule is **the same shape**. Every existing smoke test passes
`--rom`, so every existing smoke test skips the browser, and `MODE_BROWSE` would
ship with exactly the blind spot calibration had. The design carries the answer
rather than the hope:

- **`--ui-script <file>`** replays synthetic taps and key edges through
  `ui_list_feed`, making `MODE_BROWSE` and `MODE_MENU` reachable in a bounded,
  unattended run. The smoke suite gains a run that boots into the browser,
  selects a ROM, opens the menu, saves a state, loads it, and exits.
- **`ui.c` consumes `koboy_input_state`, not events**, which is what makes those
  host unit tests real rather than theatre.
- Golden images for the new faceplate, the list rendering and the extended font,
  following `chrome.c`'s existing pattern.
- The rect-coverage assertion of §7.

The general rule, restated because it generalises past this feature: **when a
code path exists only for scripted runs, ask what it is hiding.**

### Bluetooth testing

Bluetooth cannot be tested on the host at all. Both features get a hardware
verification step and a `TESTED.md` row, or the spec says plainly that they are
unverified. The author has a gamepad, so the controller path gets a real one.

## 11. Build order

0. **Bluetooth inventory spike** — *done, 2026-08-25, Appendix A. Stack inventory,
   process parentage, HIDP support and the A2DP playback path are all answered;
   CPU cost and latency are not.*
1. Measurement counters + Zelda baseline run → closes #3
2. `text.c` + `ui.c`; `main.c` mode machine → closes #14
3. `romlist.c` + `MODE_BROWSE` + `--ui-script` → closes #5
4. MENU zone: `koboy_layout`, `chrome_controls_top`, `input_take_menu_request`
5. `core_unload_rom` + `state.c` + `MODE_MENU`
6. DMG faceplate (needs `text.c` from step 2)
7. Multi-rect dirty regions, tuned from step 1
8. Bluetooth: radio bring-up in `koboy.sh`, controllers, then audio behind a flag
9. Packaging, `TESTED.md`, docs

Steps 1–7 depend on nothing in step 8, and step 8's controller half is
independent of its audio half.

## 12. Risks

- **Radio bring-up is new work inside the takeover.** `rtk_hciattach` sits in
  Nickel's process group but the launcher kills by name, so it probably
  survives — probably, not certainly, and it is unverified (§8.2). Either way
  koboy checks for `hci0` and brings it up when absent, which is also the
  Bluetooth-was-switched-off case. The concern is that this is a new thing
  `koboy.sh` does during the takeover, and the takeover is the one subsystem
  with a device-corruption incident in its history (v1 Appendix D §5).
  Bringing up a UART line discipline must not become a way to fail the restore
  path: radio bring-up happens **after** the environment gate passes, and the
  restore trap is untouched by it.
- **Audio latency may make the feature pointless.** Mitigated by shipping it off
  by default and measuring first (§8.5).
- **An orphaned audio child wedges the device's audio stack**, and killing it
  does not undo the wedge (§8.5.1, measured). This is `EVIOCGRAB`'s failure mode
  in a new place: a resource koboy holds that outlives a crash and degrades the
  device for everything afterwards, Nickel included. Mitigated by trap-based
  cleanup *and* a startup sweep for the previous run's leftovers.
- **The chrome redraw and `chrome_controls_top` must stay in lockstep.** Adding
  drawn controls without adding their terms silently re-creates the live-zone-
  under-the-game-rect bug on auto-fit panels. Guarded by a test asserting that no
  drawn control's top edge is above the value the function returns.
- **`MODE_BROWSE` is invisible to the existing test suite** unless `--ui-script`
  lands with it. Sequenced together in step 3 for that reason.
- **Save states are new user data.** They get `sram.c`'s all-or-nothing discipline
  because a half-loaded state and a half-loaded save destroy the same thing.

## 13. Open measurements

| Unknown | How it gets answered |
|---|---|
| Per-stage cost breakdown of the loop | Step 1, Zelda baseline |
| Whether the merged bbox actually costs anything in practice | Step 1, before the step 7 heuristic is tuned |
| `FIXED` in tile units, per device | Step 1; config-overridable thereafter |
| Does the SRAM path work on hardware | Step 1, Zelda (cart type `0x03`, 8 KiB) |
| A2DP latency | Step 8; audio stays off by default until answered |
| Added CPU cost of enabling gambatte's audio | Step 8 — the 2026-08-25 attempt was invalidated by the bluealsa wedge |
| Whether the PCM accepts gambatte's native 32768 Hz, or resampling is required | Step 8, same run |
| Does a gamepad reconnect after the takeover without an agent | Step 8, with the author's controller |
| Which attach helper each device family needs | Selected at runtime from what exists on the device |
| Whether `rtk_hciattach` actually survives the takeover | Bluetooth plan, task 1 — needs a real takeover, so it is measured rather than reasoned |

---

## Appendix A — Bluetooth stack inventory

Kobo Libra 2, firmware 4.38.23684, measured 2026-08-25 over SSH, read-only, with
Nickel running. This is the measurement that overturns v1 §1's "Nickel owns the
Bluetooth audio stack".

### Userspace

| Component | Path | State |
|---|---|---|
| `bluetoothd` | `/libexec/bluetooth/bluetoothd` | **running**, owns `org.bluez` |
| `bluealsa` | `/bin/bluealsa` | **running** as `bluealsa -S -i hci0` |
| `obexd` | `/libexec/bluetooth/obexd` | present |
| `dbus-daemon` | `/bin/dbus-daemon` | **running**, system bus at `/var/run/dbus/system_bus_socket` |
| attach helpers | `/sbin/hciattach`, `/sbin/rtk_hciattach` | `rtk_hciattach` running |
| BlueZ tools | `/bin/bluetoothctl`, `hcitool`, `hciconfig`, `sdptool`, `rfcomm`, `l2ping`, `bluealsa-cli`, `bt-obex` | present |
| ALSA | `/bin/aplay`, `/bin/amixer`, `/etc/alsa/conf.d/20-bluealsa.conf` | present; `pcm.bluealsa` defined, profile `a2dp`, delay 20000 |
| Libraries | `/lib/libbluetooth.so.3.19.3`, `libasound.so.2`, `libdbus-1.so.3`, `libsbc.so.1`, `libglib-2.0.so.0` | present — and koboy links **none** of them |

`bluetoothd` plugin strings include `input` and `org.bluez.Input1`, i.e. the HID
plugin that creates evdev nodes for paired controllers.

### Kernel

```
CONFIG_BT=y   CONFIG_BT_HIDP=y   CONFIG_UHID=y   CONFIG_HIDRAW=y
/proc/net/protocols: BNEP HCI HIDP L2CAP NETLINK PACKET PING RAW RFCOMM SCO ...
/dev/uhid  present (char 10:239)
```

No loadable bluetooth modules on disk — the stack is built in.

### Radio

```
hci0:  Type: Primary  Bus: UART
       BD Address: 58:B0:D4:A6:1F:C1
       UP RUNNING PSCAN ISCAN INQUIRY
       HCI Version: 4.1 (0x7)
       Manufacturer: Realtek Semiconductor Corporation (93)
       Name: 'Kobo Libra 2'
```

### Process parentage — the load-bearing measurement

| Process | PID | PPID | PGRP |
|---|---|---|---|
| `dbus-daemon --system` | 233 | 1 | 233 |
| `nickel` | 1973 | 1 | **1874** |
| `rtk_hciattach -n -s 115200 ttymxc1 rtk_h5` | 3784 | 1973 | **1874** |
| `bluealsa -S -i hci0` | 3786 | 1 | 233 |
| `bluetoothd` | 3790 | 1 | 233 |

`bluetoothd`, `bluealsa` and `dbus-daemon` share process group 233 and survive a
kill of Nickel's group 1874. `rtk_hciattach` is in Nickel's group and would not
survive a group kill — but `scripts/koboy.sh` kills by name, not by group, and
does not name it, so in practice it is reparented to init and `hci0` stays up.
Unverified: confirming it needs a real takeover. koboy checks for `hci0` and
brings it up when absent either way.

### Kobo's own configuration

```
/etc/bluetooth/main.conf
  [General] Name=Kobo   InitVolume=false
  [Policy]  ReconnectAttempts=10
            ReconnectUUIDs=00001124-0000-1000-8000-00805f9b34fb,   # HID
                           0000110b-0000-1000-8000-00805f9b34fb    # A2DP Sink
            ReconnectIntervals=1,2,4,8,16,32,64
```

D-Bus policy files present: `bluetooth.conf`, `bluealsa.conf`, `nickel.conf`,
`com-github-shermp-nickeldbus.conf`, `dhcpcd-dbus.conf`.

Names owned on the system bus at measurement time included `org.bluez`,
`com.kobo.bluetooth.Agent` and `com.kobo.bluetooth.MediaPlayer` — the latter two
are Nickel's, which is why §8.6 does not attempt pairing.

### Audio path, exercised

`aplay -l` reports **no soundcards** and `/proc/asound/cards` is empty. That is
expected rather than alarming: BlueALSA exposes a PCM through its ALSA plugin,
not a card.

With Bluetooth headphones connected (`3C:B0:ED:51:35:4C`), `bluealsa-cli
list-pcms` showed three PCMs:

```
/org/bluealsa/hci0/dev_3C_B0_ED_51_35_4C/a2dpsrc/sink     <- the one we want
/org/bluealsa/hci0/dev_3C_B0_ED_51_35_4C/hfpag/sink       <- HFP, mono, low quality
/org/bluealsa/hci0/dev_3C_B0_ED_51_35_4C/hfpag/source
```

| Attempt | Result |
|---|---|
| `aplay -D bluealsa …` | **fails**: `Unable to find definition 'defaults.bluealsa.device'` — Kobo's ALSA config never sets it |
| `aplay -D "bluealsa:DEV=3C:B0:ED:51:35:4C,PROFILE=a2dp" -f S16_LE -r 44100 -c 2 -t raw` | **works**: 2.00 s of silence, 3.09 s wall clock |

The 1.09 s difference is A2DP stream setup, which is why §8.5 opens the PCM once
per session rather than per sound.

The `hfpag` PCMs are Hands-Free Profile — mono and low bandwidth, but potentially
much lower latency. Recorded as a fallback worth measuring if A2DP latency turns
out to be unusable, not as a plan.

### Recorded failure: bluealsa wedges on an orphaned client

An `aplay` orphaned mid-stream left `bluealsa` in `futex_wait_queue_me`, not
answering D-Bus, while `bluetoothd` and the ACL link stayed healthy. `SIGKILL` on
the client did **not** recover it; only restarting `bluealsa` (`setsid
/bin/bluealsa -S -i hci0`) did. The headphones then showed `Paired=true`,
`Connected=false` — the pairing survived, the connection did not.

Design consequence in §8.5.1. This was caused by the measurement harness rather
than by koboy, which is precisely why it is worth recording: koboy would have
made the same mistake on its first crash.
