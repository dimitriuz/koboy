# koboy v2 Bluetooth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play koboy with a physical Bluetooth gamepad, and optionally hear it
through Bluetooth headphones — without linking a single new library.

**Architecture:** Every capability is reached by driving processes the device
already carries. A paired HID gamepad arrives as an ordinary `/dev/input/eventN`
node that `input.c` already decodes; audio is PCM written into an `aplay -D
bluealsa:DEV=…` child over a pipe. koboy's own dependency closure is untouched:
`fork`, `exec`, `pipe`, `write`.

**Tech Stack:** C99, libc/libm/libdl only. On-device: BlueZ 5.x
(`/libexec/bluetooth/bluetoothd` with the `input` plugin), `bluealsa`, `aplay`,
`bluealsa-cli`, `/sbin/rtk_hciattach`.

**Spec:** `docs/superpowers/specs/2026-08-25-koboy-v2-design.md` §8 and Appendix A

**Companion plan:** `docs/superpowers/plans/2026-08-25-koboy-v2-core.md`. This
plan depends on nothing in it and it depends on nothing here. Task 5 assumes
`src/config.c` gained the v2 key style; if this plan runs first, add the keys in
whatever style `config.c` currently uses.

## Global Constraints

- **C99 only, no C++.** No dependency beyond libc, libm, libdl.
- **The shipped ARM binary's dependency closure must stay exactly** `libm.so.6`,
  `libc.so.6`, `ld-linux-armhf.so.3`. `scripts/verify-core.sh` enforces it.
  The device carries `libbluetooth.so.3`, `libasound.so.2`, `libdbus-1.so.3`,
  `libsbc.so.1` and `libglib-2.0.so.0`, and **koboy links none of them.** If a
  task ever seems to need one, the answer is a subprocess, not a `-l` flag.
- **glibc 2.19** is the device floor.
- **Never `#include <linux/input.h>` in portable code.**
- **Tests must pass on the dev host.** Bluetooth itself cannot be tested there,
  so every task splits into host-testable parsing/policy logic and an explicit
  on-device verification step. Nothing here may be called done on inspection.
- **Never leave the device in a broken state.** Task 1 touches the takeover,
  which is the one subsystem with a device-corruption incident in its history
  (v1 Appendix D §5).
- **After writing any safety or regression test, break the thing it guards and
  confirm the test fails.** Record the mutant and its output.

## Device facts this plan is built on

Measured 2026-08-25, spec Appendix A. Do not re-derive these.

- `bluetoothd` (pid 3790), `bluealsa` (3786) and `dbus-daemon` (233) are **init
  children** in process group 233. Stopping Nickel does not touch them.
- `rtk_hciattach` (3784) is Nickel's child in Nickel's process group **1874**,
  but `koboy.sh` kills by name and does not name it. Whether it survives is
  **unverified** — task 1 measures it.
- Kernel: `CONFIG_BT=y`, `CONFIG_BT_HIDP=y`, `CONFIG_UHID=y`, `CONFIG_HIDRAW=y`;
  `/dev/uhid` exists; `HIDP` is registered.
- `bluetoothd` ships the `input` plugin (`org.bluez.Input1`), so a paired HID
  device gets an evdev node.
- `/etc/bluetooth/main.conf` sets
  `ReconnectUUIDs=00001124-…` (HID) `,0000110b-…` (A2DP Sink),
  `ReconnectAttempts=10`.
- A bare `aplay -D bluealsa` **fails**: Kobo's
  `/etc/alsa/conf.d/20-bluealsa.conf` never sets `defaults.bluealsa.device`.
  `aplay -D "bluealsa:DEV=<MAC>,PROFILE=a2dp" -f S16_LE -r 44100 -c 2 -t raw`
  works — 2.00 s of audio took 3.09 s wall clock, so **stream setup is ~1.1 s**.
- An orphaned `aplay` wedged `bluealsa` so it stopped answering D-Bus, and
  `SIGKILL` on the client **did not** recover it. Only restarting the daemon did.

---

### Task 1: Radio bring-up, and measuring what the takeover really does

**Files:**
- Modify: `scripts/koboy.sh`
- Create: `tests/test_launcher_bt.sh`

**Interfaces:**
- Consumes: nothing.
- Produces: `koboy.sh` guarantees `hci0` exists before `exec`ing koboy, or logs
  why it does not.

- [ ] **Step 1: ON DEVICE — measure whether `hci0` survives the takeover**

This is the unverified claim the spec flags, and everything else in the task
depends on it. Do it **before** writing code.

With Bluetooth ON in Nickel and the device on WiFi, over SSH:

```sh
hciconfig hci0 | head -3
ps | grep -E '[r]tk_hciattach|[b]luetoothd|[b]luealsa'
```

Record the output. Then launch koboy from **NickelMenu** (not SSH — the
environment gate exists for a reason and an SSH launch refuses to restart
Nickel), and while it is running, from a second SSH session:

```sh
hciconfig hci0 | head -3
ps | grep -E '[r]tk_hciattach|[b]luetoothd|[b]luealsa'
```

Record whether `rtk_hciattach` is still alive and whether `hci0` is still
`UP RUNNING`. Exit koboy normally and confirm Nickel comes back **without a
reboot** and that `/mnt/onboard/.kobo/version` still holds the real serial —
that check is not optional, it is v1 Appendix D §5's whole lesson.

Write the answer into the spec's §8.2 and into the open-measurements table,
replacing "unverified".

- [ ] **Step 2: Add radio bring-up to the launcher**

Whatever step 1 found, the code is the same, because the far more common case is
Bluetooth having been switched off before launch. In `scripts/koboy.sh`, after
the "Nickel stopped" block and **before** the 8bpp switch:

```sh
# ------------------------------------------------------------------ bluetooth
#
# The controller and audio paths both need an HCI interface. bluetoothd and
# bluealsa are init children in dbus-daemon's process group and are untouched by
# stopping Nickel; the UART attach helper is Nickel's child, though killall by
# name above does not name it. Rather than reason about which, ask the kernel.
#
# This is also -- and mostly -- the case where the user simply had Bluetooth
# switched off, which is the common one and the one that would otherwise leave
# somebody wondering why their gamepad does nothing.
#
# Deliberately non-fatal in every branch: koboy plays fine with touch and the
# page-turn buttons, and failing to start over an optional radio would be a far
# worse bug than not having it.
bt_up() {
    if [ -d /sys/class/bluetooth/hci0 ]; then
        log "bluetooth: hci0 already present"
        return 0
    fi
    if [ "$KOBOY_NO_BT" = "1" ]; then
        log "bluetooth: KOBOY_NO_BT=1, not bringing the radio up"
        return 0
    fi

    # Which helper, by what exists rather than by a model table -- the same
    # capability-detection rule the rest of the project follows. Nickel's own
    # strings name Realtek, NXP and Cypress variants.
    helper=""
    for h in /sbin/rtk_hciattach /sbin/hciattach; do
        [ -x "$h" ] && { helper="$h"; break; }
    done
    if [ -z "$helper" ]; then
        log "bluetooth: no hciattach helper on this device, skipping"
        return 0
    fi

    log "bluetooth: bringing hci0 up with $helper"
    # setsid so it cannot be orphaned into this script's session and die with
    # it -- the same mistake that wedged bluealsa during the measurement
    # session that produced Appendix A.
    setsid "$helper" -n -s 115200 ttymxc1 rtk_h5 >>"$LOG" 2>&1 &
    BT_ATTACH_PID=$!

    i=0
    while [ ! -d /sys/class/bluetooth/hci0 ]; do
        [ "$i" -ge 20 ] && { log "bluetooth: WARNING hci0 did not appear in 5s"; return 0; }
        usleep 250000 2>/dev/null || sleep 1
        i=$((i + 1))
    done
    hciconfig hci0 up >>"$LOG" 2>&1
    log "bluetooth: hci0 up after $((i * 250))ms"
}
bt_up
```

- [ ] **Step 3: Tear it down only if we brought it up**

In `restore()`, **before** `wifi_down`:

```sh
    # Only what we started. If the helper was already running when koboy
    # launched, it belongs to Nickel and killing it would break Bluetooth for
    # the session the user returns to.
    if [ -n "$BT_ATTACH_PID" ]; then
        log "restore: stopping the hciattach we started (pid $BT_ATTACH_PID)"
        kill -TERM "$BT_ATTACH_PID" 2>/dev/null
    fi
```

- [ ] **Step 4: Write the launcher assertions**

Create `tests/test_launcher_bt.sh`. It cannot run the launcher, so it asserts on
its text — the same approach `tests/test_dist.sh` already takes for launcher
safety properties:

```sh
#!/bin/sh
# Launcher safety assertions for the Bluetooth bring-up. These are text
# assertions because the launcher cannot be executed on the host: it stops
# Nickel. tests/test_dist.sh already asserts launcher properties this way.
set -e
S=scripts/koboy.sh
fail() { echo "FAIL: $1"; exit 1; }

# The bring-up must sit AFTER Nickel is stopped: bluetoothd survives the
# takeover, but starting a UART line discipline while Nickel still owns the
# device is asking for a fight.
stop_line=$(grep -n 'stopping Nickel' "$S" | head -1 | cut -d: -f1)
bt_line=$(grep -n '^bt_up$' "$S" | head -1 | cut -d: -f1)
[ -n "$stop_line" ] && [ -n "$bt_line" ] || fail "could not locate both blocks"
[ "$bt_line" -gt "$stop_line" ] || fail "bt_up runs before Nickel is stopped"

# The helper must be started detached. An orphan holding a Bluetooth resource
# is exactly what wedged bluealsa during the Appendix A measurements.
grep -q 'setsid "$helper"' "$S" || fail "hciattach is not started with setsid"

# Teardown must be conditional on BT_ATTACH_PID. Killing a helper we did not
# start breaks Bluetooth for the Nickel session the user returns to.
grep -q 'if \[ -n "$BT_ATTACH_PID" \]' "$S" || fail "unconditional hciattach teardown"

# Bring-up must never be able to fail the launch.
grep -q 'KOBOY_NO_BT' "$S" || fail "no KOBOY_NO_BT escape hatch"

# And it must not touch the restore trap's existing duties.
grep -q 'trap .finish. EXIT' "$S" || fail "EXIT trap was disturbed"
echo "ok: launcher bluetooth assertions"
```

- [ ] **Step 5: Run it, and verify each assertion is real (mutants)**

Run: `sh tests/test_launcher_bt.sh`
Expected: `ok: launcher bluetooth assertions`

Then, one at a time: move `bt_up` above the Nickel stop; drop `setsid`; make the
teardown unconditional; delete the `KOBOY_NO_BT` branch. Each must make the
script FAIL with its specific message. Revert each. Record all four outputs.

- [ ] **Step 6: ON DEVICE — verify bring-up from Bluetooth-off**

Switch Bluetooth **off** in Nickel. Launch koboy from NickelMenu. From SSH,
confirm `hci0` appears. Exit koboy and confirm Nickel returns without a reboot
and `/mnt/onboard/.kobo/version` is intact.

- [ ] **Step 7: Commit**

```bash
git add scripts/koboy.sh tests/test_launcher_bt.sh
git commit -m "feat: bring the Bluetooth radio up during the takeover

bluetoothd and bluealsa are init children and survive stopping Nickel;
the UART attach helper is Nickel's child, though killall by name does not
name it. Rather than reason about which, koboy.sh asks the kernel for
hci0 and starts a helper only if there is none -- which is also, and
mostly, the case where the user simply had Bluetooth switched off.

Non-fatal in every branch: koboy plays fine on touch and the page-turn
buttons, and refusing to start over an optional radio would be a worse
bug than not having it. Teardown is conditional on having started it,
because killing a helper we did not start breaks Bluetooth for the Nickel
session the user comes back to.

setsid, because an orphan holding a Bluetooth resource is exactly what
wedged bluealsa during the measurements this is built on."
```

---

### Task 2: Adopt a controller that appears after startup

**Files:**
- Create: `src/btinput.h`, `src/btinput.c`
- Create: `tests/test_btinput.c`
- Modify: `src/platform_kobo.c`

**Interfaces:**
- Consumes: `koboy_ev` and the `KOBOY_EV_*` constants (`src/input.h`).
- Produces:
  - `bool btinput_is_gamepad(const char *devices_block)` — pure, parses one
    `/proc/bus/input/devices` record
  - `int btinput_parse_handlers(const char *devices_block, char *out, size_t n)` —
    extracts the `eventN` name
  - `int btinput_scan(char *out_node, size_t n)` — returns 1 if a gamepad node
    was found, 0 if none, -1 on error

- [ ] **Step 1: Write the failing test**

Create `tests/test_btinput.c`:

```c
#include "test.h"
#include "btinput.h"
#include <string.h>

/* Real records, in the exact shape /proc/bus/input/devices emits. The three
   non-gamepad ones are copied from the verified Libra 2 (spec Appendix A), so
   the filter is tested against the hardware it has to coexist with. */
static const char GPIO_KEYS[] =
    "I: Bus=0019 Vendor=0001 Product=0001 Version=0100\n"
    "N: Name=\"gpio-keys\"\n"
    "H: Handlers=event0 \n"
    "B: PROP=0\n"
    "B: EV=100013\n"
    "B: KEY=6 0 0 100000 0 8000000 0\n";

static const char TOUCH[] =
    "I: Bus=0018 Vendor=0000 Product=0000 Version=0000\n"
    "N: Name=\"Elan Touchscreen\"\n"
    "H: Handlers=kbd mouse0 event1 \n"
    "B: PROP=2\n"
    "B: EV=b\n"
    "B: ABS=ee18000 1000003\n";

static const char ACCEL[] =
    "I: Bus=0018 Vendor=001b Product=0000 Version=0000\n"
    "N: Name=\"kx122-accel\"\n"
    "H: Handlers=event2 \n"
    "B: PROP=0\n"
    "B: EV=9\n"
    "B: ABS=7\n";

/* A Bluetooth HID gamepad: Bus=0005 is BUS_BLUETOOTH, and it advertises both
   EV_KEY and EV_ABS with BTN_GAMEPAD-range keys. */
static const char PAD[] =
    "I: Bus=0005 Vendor=057e Product=2009 Version=0001\n"
    "N: Name=\"Pro Controller\"\n"
    "H: Handlers=event3 js0 \n"
    "B: PROP=0\n"
    "B: EV=20000b\n"
    "B: KEY=7fdb000000000000 0 0 0 0\n"
    "B: ABS=3f\n";

/* A Bluetooth keyboard is NOT a gamepad: it has EV_KEY but no gamepad buttons
   and no absolute axes. Adopting it would send stray keys into the game. */
static const char BT_KBD[] =
    "I: Bus=0005 Vendor=05ac Product=0255 Version=0011\n"
    "N: Name=\"Magic Keyboard\"\n"
    "H: Handlers=sysrq kbd event4 \n"
    "B: PROP=0\n"
    "B: EV=120013\n"
    "B: KEY=e0ff0f 0 0 0 0\n";

TEST_MAIN({
    CHECK_EQ_INT(btinput_is_gamepad(PAD), 1);

    /* None of the device's own nodes may be mistaken for a controller. If any
       of these were adopted koboy would read the touchscreen twice, or feed
       the accelerometer into the joypad. */
    CHECK_EQ_INT(btinput_is_gamepad(GPIO_KEYS), 0);
    CHECK_EQ_INT(btinput_is_gamepad(TOUCH), 0);
    CHECK_EQ_INT(btinput_is_gamepad(ACCEL), 0);
    CHECK_EQ_INT(btinput_is_gamepad(BT_KBD), 0);

    /* Handler extraction picks eventN and ignores js0, kbd, mouse0. */
    char node[64];
    CHECK_EQ_INT(btinput_parse_handlers(PAD, node, sizeof node), 1);
    CHECK(strcmp(node, "event3") == 0);
    CHECK_EQ_INT(btinput_parse_handlers(TOUCH, node, sizeof node), 1);
    CHECK(strcmp(node, "event1") == 0);

    /* A record with no event handler is reported, not guessed at. */
    CHECK_EQ_INT(btinput_parse_handlers(
        "N: Name=\"x\"\nH: Handlers=js0 \n", node, sizeof node), 0);

    /* Malformed input must not overrun. */
    char tiny[4];
    CHECK_EQ_INT(btinput_parse_handlers(PAD, tiny, sizeof tiny), 0);
    CHECK_EQ_INT(btinput_is_gamepad(""), 0);
    CHECK_EQ_INT(btinput_is_gamepad("H: Handlers="), 0);
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_btinput`
Expected: FAIL — `fatal error: btinput.h: No such file or directory`

- [ ] **Step 3: Write the header and implementation**

Create `src/btinput.h`:

```c
#ifndef KOBOY_BTINPUT_H
#define KOBOY_BTINPUT_H
#include <stdbool.h>
#include <stddef.h>

/* Finding a Bluetooth gamepad, which to koboy is just another key device.

   bluetoothd's `input` plugin turns a paired HID device into an ordinary
   /dev/input/eventN node, and input.c already decodes those -- so the whole of
   "controller support" is finding the right node and reading it. No libbluetooth,
   no D-Bus, no new dependency: this parses /proc/bus/input/devices.

   The parsing is split out and pure so the filter is tested against the real
   records of the verified device rather than against a device that must be
   present. */

/* True for one /proc/bus/input/devices record that looks like a gamepad:
   BUS_BLUETOOTH, EV_KEY and EV_ABS, and at least one key in the BTN_GAMEPAD
   range. A Bluetooth KEYBOARD must not match -- adopting one would push stray
   keys into the running game. */
bool btinput_is_gamepad(const char *record);

/* Extracts the eventN handler from a record into `out`. Returns 1 on success,
   0 if there is no event handler or `out` is too small. */
int  btinput_parse_handlers(const char *record, char *out, size_t n);

/* Scans /proc/bus/input/devices for the first gamepad and writes its node path
   ("/dev/input/eventN") into out_node. 1 = found, 0 = none, -1 = cannot read. */
int  btinput_scan(char *out_node, size_t n);
#endif
```

Create `src/btinput.c` implementing:
- `btinput_is_gamepad`: find the `B: EV=` line and parse its hex mask; require
  bit 1 (`EV_KEY`) **and** bit 3 (`EV_ABS`). Find `I: Bus=` and require `0005`.
  Find `B: KEY=` and require a set bit in the `BTN_GAMEPAD` range (0x130–0x13F)
  — the KEY mask is printed as space-separated 64-bit hex words, most
  significant first, so index carefully and comment the indexing.
- `btinput_parse_handlers`: scan the `H: Handlers=` line for a whitespace-
  delimited token beginning `event`; bounds-check against `n` before copying.
- `btinput_scan`: read `/proc/bus/input/devices` whole, split on blank lines,
  call the two above.

Every bound check gets a comment saying it is live: this parses a kernel file
whose format varies with kernel version, so "cannot happen" is not available.

- [ ] **Step 4: Run test to verify it passes**

Run: `make build/test_btinput && ./build/test_btinput`
Expected: PASS.

- [ ] **Step 5: Verify the keyboard exclusion is real (mutant)**

Delete the `BTN_GAMEPAD`-range requirement from `btinput_is_gamepad`.
Expected: `CHECK_EQ_INT(btinput_is_gamepad(BT_KBD), 0)` FAILS. Revert.

This mutant matters: without the check, pairing a Bluetooth keyboard while
playing sends its keypresses into the game.

- [ ] **Step 6: Verify the bus check is real (mutant)**

Delete the `Bus=0005` requirement.
Expected: the `GPIO_KEYS` check FAILS — koboy would open the page-turn key node
a second time. Revert. Record both outputs.

- [ ] **Step 7: Open the node in the Kobo backend**

In `src/platform_kobo.c`, where input nodes are opened, add a gamepad slot:

```c
/* A gamepad is just another key device, so it goes through the same
   input_feed_key path as the page-turn buttons. Opened NON-BLOCKING like the
   others, and never grabbed: v1 Appendix C established that the power button
   cannot be excluded from a grab on this hardware, and there is no reason to
   take exclusive ownership of a device the user may want back. */
```

Re-scan for a gamepad every few seconds while running, so a controller
connected mid-game is adopted — BlueZ's `ReconnectUUIDs` policy reconnects HID
devices on its own schedule, which will frequently land after koboy has started.
Rate-limit the rescan to at most once per second so it never becomes a per-frame
`open()` on `/proc`.

- [ ] **Step 8: ON DEVICE — verify with the real controller**

Pair the gamepad **before** launching koboy — via Nickel's Bluetooth settings,
or over SSH with `bluetoothctl` (koboy does not pair; spec §8.6). Confirm it
appears:

```sh
cat /proc/bus/input/devices | grep -A6 -i 'Bus=0005'
```

Launch koboy from NickelMenu and confirm from the log that the gamepad node was
found. Then verify in a game that its buttons reach the emulator.

If the pad connects only *after* koboy starts, confirm the rescan picks it up.

- [ ] **Step 9: Commit**

```bash
git add src/btinput.h src/btinput.c tests/test_btinput.c src/platform_kobo.c
git commit -m "feat: adopt a Bluetooth gamepad as another key device

bluetoothd's input plugin turns a paired HID device into an ordinary
evdev node, and input.c already decodes those -- so controller support is
finding the right node, not speaking Bluetooth. No libbluetooth, no
D-Bus, no new dependency.

v1 called the touch d-pad the main threat to playability: no tactile
feedback on a panel that confirms input ~50ms late. A physical gamepad
deletes that problem rather than mitigating it.

Rescanned while running, because BlueZ reconnects HID devices on its own
schedule and that frequently lands after koboy has started.

Two mutants recorded. Dropping the BTN_GAMEPAD-range check adopts a
Bluetooth keyboard and pushes its keypresses into the game; dropping the
Bus=0005 check re-opens the device's own page-turn key node."
```

---

### Task 3: Gamepad calibration

**Files:**
- Modify: `src/calib.h`, `src/calib.c`
- Modify: `src/config.h`, `src/config.c`
- Modify: `src/input.c`
- Modify: `tests/test_calib.c`

**Interfaces:**
- Consumes: `btinput_scan` (task 2).
- Produces:
  - `koboy_config.key_up, key_down, key_left, key_right, key_start, key_select`
  - calibration stages for all eight buttons, skippable

- [ ] **Step 1: Write the failing test**

Extend `tests/test_calib.c` to drive the full eight-button sequence, asserting:
- Each stage's prompt names the button being asked for.
- `KOBOY_KEY_POWER` (116) is rejected at **every** stage, not only at A and B.
  Power is the quit key, and a calibration that let a user bind it would make
  the device unquittable.
- The same code assigned twice is rejected, so a user cannot bind one button to
  two actions and lose a direction with no way to tell why.
- A touch escape at any stage keeps the built-in defaults for the remaining
  stages and leaves no zero sentinel — the exact failure `calib_escape` exists
  for, since a zeroed code makes `input_feed_key` ignore that key for the whole
  session.
- Calibration is skipped entirely when a gamepad is absent and the two
  page-turn defaults are already valid, so a touch-only Kobo and an
  already-working Libra 2 are not asked eight questions they cannot answer.

- [ ] **Step 2: Run to verify it fails, then implement**

Run: `make build/test_calib && ./build/test_calib`
Expected: FAIL.

Extend `calib.c`'s stage machine from two stages to eight, keeping its existing
structure and its power-button rejection, and generalising that rejection to
apply at every stage. Add the six new config keys with the same ini
parse/save treatment `key_a` and `key_b` already get, and extend
`config_save_keys` to write all eight — keeping it idempotent, which is what its
filter exists for.

In `input.c`, extend `input_feed_key` to map the six new codes to
`KOBOY_BTN_UP/DOWN/LEFT/RIGHT/START/SELECT`, keeping the existing behaviour that
an unset (zero) code matches nothing.

- [ ] **Step 3: Verify the power rejection is real (mutant)**

Remove the power-button rejection from one of the new stages.
Expected: the test FAILS for that stage. Revert. Record the output.

This is the mutant that matters: a user who binds power to "left" cannot quit,
and on a device where a stuck app looks like a brick that is the worst outcome
this project can produce.

- [ ] **Step 4: ON DEVICE — calibrate the real controller**

Delete `key_a`/`key_b` from `koboy.ini` to force calibration, launch from
NickelMenu with the gamepad connected, and walk the eight prompts. Confirm
`koboy.ini` ends with exactly one line per key and that the game is playable on
the pad afterwards.

Then relaunch and confirm calibration does **not** run again.

- [ ] **Step 5: Commit**

```bash
git add src/calib.h src/calib.c src/config.h src/config.c src/input.c \
        tests/test_calib.c
git commit -m "feat: calibrate all eight buttons, for gamepads

v1 learned two buttons on first run because the device had two. A gamepad
has a d-pad, A, B, Start and Select, and the same 'learn it, do not table
it' approach covers every controller nobody has tested.

The power-button rejection now applies at EVERY stage, not just A and B.
A user who bound power to a direction could not quit, and on a device
where a stuck app is indistinguishable from a brick that is the worst
outcome available.

Skipped entirely when no gamepad is present and the page-turn defaults
are valid, so a touch-only Kobo is not asked eight questions it cannot
answer."
```

---

### Task 4: The audio pipe

**Files:**
- Create: `src/btaudio.h`, `src/btaudio.c`
- Create: `tests/test_btaudio.c`
- Modify: `src/core.c` (stop reporting audio off; forward the batch callback)
- Modify: `src/config.h`, `src/config.c`, `config/koboy.ini` (`audio`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `int btaudio_parse_pcm_dev(const char *bluealsa_cli_output, char *out, size_t n)` — pure
  - `koboy_btaudio *btaudio_open(int rate)` / `void btaudio_close(koboy_btaudio *)`
  - `void btaudio_write(koboy_btaudio *, const int16_t *frames, size_t n)` — never blocks
  - `koboy_config.audio` (default **false**)

- [ ] **Step 1: Write the failing test for the pure part**

Create `tests/test_btaudio.c`:

```c
#include "test.h"
#include "btaudio.h"
#include <string.h>

/* Real bluealsa-cli list-pcms output, captured from the device 2026-08-25. */
static const char PCMS[] =
    "/org/bluealsa/hci0/dev_3C_B0_ED_51_35_4C/hfpag/sink\n"
    "/org/bluealsa/hci0/dev_3C_B0_ED_51_35_4C/hfpag/source\n"
    "/org/bluealsa/hci0/dev_3C_B0_ED_51_35_4C/a2dpsrc/sink\n";

TEST_MAIN({
    char mac[32];

    /* The A2DP SINK is the one to write to. Picking hfpag would give mono
       8kHz telephony audio, which is not what anyone means by game sound. */
    CHECK_EQ_INT(btaudio_parse_pcm_dev(PCMS, mac, sizeof mac), 1);
    CHECK(strcmp(mac, "3C:B0:ED:51:35:4C") == 0);

    /* No A2DP sink means no audio, reported rather than guessed. This is the
       normal case: no headphones connected. */
    CHECK_EQ_INT(btaudio_parse_pcm_dev(
        "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/hfpag/sink\n",
        mac, sizeof mac), 0);
    CHECK_EQ_INT(btaudio_parse_pcm_dev("", mac, sizeof mac), 0);

    /* bluealsa unresponsive: bluealsa-cli prints an error to stderr and
       nothing to stdout. Must not be parsed as a device. Measured for real --
       an orphaned aplay wedged bluealsa's D-Bus during the Appendix A session
       and list-pcms timed out. */
    CHECK_EQ_INT(btaudio_parse_pcm_dev(
        "bluealsa-cli: E: CMD \"list-pcms\": Couldn't get BlueALSA PCM list\n",
        mac, sizeof mac), 0);

    /* Truncation is refused, never half-written: the MAC goes straight into an
       exec argv. */
    char tiny[8];
    CHECK_EQ_INT(btaudio_parse_pcm_dev(PCMS, tiny, sizeof tiny), 0);

    /* A malformed path must not be turned into a plausible-looking MAC. */
    CHECK_EQ_INT(btaudio_parse_pcm_dev(
        "/org/bluealsa/hci0/dev_NOT_A_MAC/a2dpsrc/sink\n", mac, sizeof mac), 0);
})
```

- [ ] **Step 2: Run to verify it fails**

Run: `make build/test_btaudio`
Expected: FAIL — `fatal error: btaudio.h: No such file or directory`

- [ ] **Step 3: Implement**

`btaudio_parse_pcm_dev` finds a line containing `/a2dpsrc/sink`, extracts the
`dev_XX_XX_XX_XX_XX_XX` component, converts underscores to colons, and
**validates all six octets as hex** before accepting — the result goes into an
`exec` argv.

`btaudio_open`:
1. Runs `bluealsa-cli list-pcms` via `popen`, reads stdout, parses the MAC.
   Returns NULL if there is none.
2. Builds `bluealsa:DEV=<MAC>,PROFILE=a2dp`.
3. `pipe()` + `fork()` + `execlp("aplay", "aplay", "-D", pcm, "-f", "S16_LE",
   "-r", rate_str, "-c", "2", "-t", "raw", "-q", NULL)`, with the read end on
   the child's stdin.
4. Sets the write end `O_NONBLOCK`.
5. **Records the child pid**, which task 5 needs.

```c
/* The PCM is opened ONCE and held for the session. Measured on the device:
   2.00s of audio took 3.09s wall clock, so stream setup is ~1.1s. Opening per
   sound effect is not slow, it is impossible. */
```

`btaudio_write` writes into a bounded ring buffer and drains it with
non-blocking `write()`. On `EAGAIN` it **drops** the oldest frames:

```c
/* Dropped audio, never a stalled emulator. The main loop's budget is one
   frame; blocking here for a Bluetooth pipe would stall emulation behind a
   radio. Audio glitches; the game does not. */
```

Ignore `SIGPIPE` (set `SIG_IGN` once at open) so a dead `aplay` cannot kill
koboy.

- [ ] **Step 4: Run to verify it passes**

Run: `make build/test_btaudio && ./build/test_btaudio`
Expected: PASS.

- [ ] **Step 5: Verify the MAC validation is real (mutant)**

Remove the hex validation of the six octets.
Expected: the malformed-path check FAILS, i.e. `NOT:A:MAC` is handed to `exec`.
Revert. Record the output.

- [ ] **Step 6: Turn the core's audio on, behind a config key**

In `src/core.c`, make the `GET_AUDIO_VIDEO_ENABLE` response depend on whether
audio is enabled rather than always reporting audio off, and forward
`retro_set_audio_sample_batch` to a callback instead of discarding. **Keep the
discarding stub installed when audio is off** — v1's spec is explicit that a
core calling a NULL pointer crashes, and that has not changed.

Add `koboy_config.audio`, defaulting to **false**, with an ini entry:

```
# Bluetooth audio through the device's own bluealsa and aplay. OFF BY DEFAULT,
# and deliberately so rather than by oversight: the path is verified to work
# (2s of audio played over A2DP on a Libra 2, 2026-08-25) but its latency, its
# CPU cost on a single-core A9, and its behaviour when a panel refresh stalls
# the loop are all unmeasured. Turn it on if you want to help measure them.
# Requires headphones already paired and connected -- koboy never pairs.
audio = false
```

- [ ] **Step 7: ON DEVICE — measure the three unknowns**

With headphones connected and `audio = true`, play a game and record:

1. **Latency** — how far behind the picture the sound is. If it is bad enough to
   be worse than silence, say so and leave the default off.
2. **CPU** — compare the `stages` line's `core=` figure against a run with
   `audio = false`. This is the number the 2026-08-25 attempt failed to obtain
   because an orphaned `aplay` wedged `bluealsa`.
3. **Whether 32768 Hz works**, or whether resampling to 44100 is required.

Record all three in `TESTED.md`. **Do not flip the default to true** unless the
numbers justify it.

- [ ] **Step 8: Commit**

```bash
git add src/btaudio.h src/btaudio.c tests/test_btaudio.c src/core.c \
        src/config.h src/config.c config/koboy.ini
git commit -m "feat: Bluetooth audio through the device's own bluealsa and aplay

koboy links no new library. It runs bluealsa-cli to find the A2DP sink,
then pipes S16 PCM into aplay -D bluealsa:DEV=<MAC>,PROFILE=a2dp. The
device's ALSA config never sets defaults.bluealsa.device, so a bare
-D bluealsa fails -- the MAC must be passed, which is why there is a
parser and why it validates all six octets before they reach exec.

The PCM is opened once per session: stream setup measured ~1.1s, so
opening per sound is not slow but impossible.

Writes are non-blocking and drop on overrun. Blocking would stall
emulation behind a radio; audio glitches, the game does not.

Ships OFF by default. The path is verified to work; its latency, its CPU
cost and its behaviour under a stalled loop are not."
```

---

### Task 5: Do not wedge the device's audio stack

This task exists because of a measured failure, not a hypothetical one. Spec
§8.5.1: an orphaned `aplay` left `bluealsa` in `futex_wait_queue_me` refusing
D-Bus, and `SIGKILL` on the client **did not** recover it — only restarting the
daemon did. That is `EVIOCGRAB`'s failure mode in a new place: a resource koboy
holds that outlives a crash and degrades the device for everything afterwards,
Nickel included.

**Files:**
- Modify: `src/btaudio.c` (`btaudio_close`, signal-handler-safe teardown)
- Modify: `src/main.c` (signal handler, exit path)
- Modify: `scripts/koboy.sh` (startup sweep, trap cleanup)
- Modify: `tests/test_launcher_bt.sh`

**Interfaces:**
- Consumes: `btaudio_open`'s recorded child pid (task 4).
- Produces: `void btaudio_kill_async(void)` — async-signal-safe, for handlers.

- [ ] **Step 1: Add the assertions first**

Extend `tests/test_launcher_bt.sh`:

```sh
# A crash can orphan the aplay child, and an orphan holding the A2DP PCM wedges
# bluealsa so hard that killing the orphan does not recover it -- measured, spec
# 8.5.1. Trap cleanup cannot be sufficient on its own, because a hard crash
# skips traps. So the launcher ALSO sweeps on startup: recovering from the last
# run's crash is cheaper than preventing every crash.
grep -q 'aplay' "$S" || fail "launcher never mentions aplay"
sweep=$(grep -n 'stale aplay' "$S" | head -1 | cut -d: -f1)
exec_line=$(grep -n '^"\$KOBOY" --config' "$S" | head -1 | cut -d: -f1)
[ -n "$sweep" ] && [ -n "$exec_line" ] || fail "could not locate sweep and exec"
[ "$sweep" -lt "$exec_line" ] || fail "aplay sweep runs after koboy starts"

# And the trap must clean up on the way out.
grep -A30 '^restore() {' "$S" | grep -q 'aplay' \
    || fail "restore() does not clean up the audio child"
```

- [ ] **Step 2: Run to verify it fails**

Run: `sh tests/test_launcher_bt.sh`
Expected: FAIL — `launcher never mentions aplay`.

- [ ] **Step 3: Sweep on startup**

In `scripts/koboy.sh`, inside `bt_up` or immediately after it, and **before**
the `exec` of koboy:

```sh
# Clear any stale aplay left by a previous crashed run. A hard crash skips
# every trap, so trap-based cleanup alone cannot be sufficient -- and an orphan
# holding the A2DP PCM wedges bluealsa badly enough that killing the orphan
# afterwards does not recover it (measured; see the v2 spec 8.5.1). Recovering
# from the previous run's crash is cheaper than preventing every crash.
stale=$(ps | awk '/[a]play -D bluealsa/ {print $1}')
if [ -n "$stale" ]; then
    log "bluetooth: clearing stale aplay ($stale) from a previous run"
    kill -9 $stale 2>/dev/null
fi
```

Note the `[a]play` bracket idiom: without it the `awk` matches its own command
line, and during the measurement session that exact mistake made a cleanup
script kill itself instead of its target.

- [ ] **Step 4: Clean up in the trap**

In `restore()`, beside the conditional `hciattach` teardown:

```sh
    # The audio child, if koboy left one. Same reasoning as the startup sweep.
    leftover=$(ps | awk '/[a]play -D bluealsa/ {print $1}')
    [ -n "$leftover" ] && { log "restore: killing audio child ($leftover)"; kill -9 $leftover 2>/dev/null; }
```

- [ ] **Step 5: Clean up in the process too**

In `src/btaudio.c`, `btaudio_close` closes the pipe, `SIGTERM`s the child,
`waitpid`s with a short bounded retry, then `SIGKILL`s. Add:

```c
/* Async-signal-safe: kill(2) and _exit(2) only, no allocation, no stdio. The
   signal handler's job is the minimum -- v1's spec is explicit that real work
   in a SIGSEGV handler is its own hazard -- but leaving this child behind
   wedges the device's audio stack for everything afterwards, Nickel included,
   so it earns its place beside the SRAM flush and the grab release. */
void btaudio_kill_async(void);
```

backed by a file-scope `volatile sig_atomic_t g_aplay_pid`.

In `src/main.c`, call `btaudio_kill_async()` from the signal handler and
`btaudio_close()` on the normal exit path, next to the final SRAM flush.

- [ ] **Step 6: Run the assertions and verify they are real (mutants)**

Run: `sh tests/test_launcher_bt.sh`
Expected: `ok`.

Then: move the sweep after the `exec` line; delete the `restore()` cleanup. Each
must FAIL with its own message. Revert both. Record the outputs.

- [ ] **Step 7: ON DEVICE — reproduce the wedge and prove the fix**

With `audio = true` and headphones connected:

1. Launch koboy, start a game, confirm sound.
2. From SSH, `kill -9` koboy itself — simulating the crash that skips every trap.
3. Confirm an orphaned `aplay` exists and that `bluealsa-cli list-pcms` now
   times out, i.e. the wedge reproduces.
4. Launch koboy again from NickelMenu.
5. Confirm the launcher's sweep cleared the orphan, and record **whether
   `bluealsa` recovered on its own or still needed a restart.**

Step 5 is the honest part. The measurement says killing the client was *not*
enough to unwedge `bluealsa`. If that holds, say so in `TESTED.md` and in the
spec: the sweep prevents the wedge from a *clean* orphan but may not repair an
already-wedged daemon, and koboy deliberately does not restart a system daemon
it does not own (spec §8.5.1). Do not claim the fix is complete if it is not.

- [ ] **Step 8: Commit**

```bash
git add src/btaudio.c src/main.c scripts/koboy.sh tests/test_launcher_bt.sh
git commit -m "fix: never leave an aplay child wedging the audio stack

Measured, not hypothetical: an orphaned aplay left bluealsa in
futex_wait_queue_me refusing D-Bus, and SIGKILL on the client did not
recover it -- only restarting the daemon did. That is EVIOCGRAB's failure
mode in a new place, a resource koboy holds that outlives a crash and
degrades the device for everything after it, Nickel included.

Three layers, because a hard crash skips traps: an async-signal-safe kill
in the handler, a trap cleanup in restore(), and a startup sweep for the
previous run's leftovers. Recovering from the last crash is cheaper than
preventing every crash.

The awk uses the [a]play bracket idiom. Without it the pattern matches
its own command line -- during the measurement session that exact mistake
made a cleanup script kill itself instead of its target."
```

---

### Task 6: Device verification and honest documentation

**Files:**
- Modify: `TESTED.md`, `README.md`, `CLAUDE.md`
- Modify: `docs/superpowers/specs/2026-08-25-koboy-v2-design.md`
- Modify: `docs/device-workflow.md`

- [ ] **Step 1: Run the full matrix on hardware**

One session per row, from NickelMenu, recording `koboy.log` each time:

| Run | Asserts |
|---|---|
| Gamepad, audio off | Controller playable; clean exit; version file intact |
| No gamepad, audio off | Touch and page-turn buttons unaffected by any of this |
| Gamepad + headphones, audio on | Sound plays; record latency, `core=` cost, sample rate |
| Bluetooth off at launch | `bt_up` brings `hci0` up; clean exit |
| `KOBOY_NO_BT=1` | Radio untouched; koboy still plays |
| Kill -9 mid-session with audio on | Next launch sweeps the orphan (task 5 step 7) |

After **every** run, confirm Nickel returns without a reboot and
`/mnt/onboard/.kobo/version` still holds the real serial. That check is not
ceremony: rewriting it is the one mistake this project has already made, and it
takes a reboot to repair.

- [ ] **Step 2: Verify the closure one final time**

```bash
export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make kobo && bash scripts/verify-core.sh build/koboy-arm
```

Expected: exactly `libm.so.6`, `libc.so.6`, `ld-linux-armhf.so.3`.

**This is the whole architectural claim of this plan.** The device carries
`libbluetooth`, `libasound`, `libdbus-1`, `libsbc` and `libglib` — and koboy
gained Bluetooth audio and controller support without linking one of them. If
this check fails, something took a shortcut and must be undone rather than
allowlisted.

- [ ] **Step 3: Write down what is true**

`TESTED.md`: a Bluetooth section with the gamepad model tested, the headphone
model, the measured latency and CPU cost, and — plainly — everything still
unverified. One device is verified; Bluetooth on any other Kobo is unmeasured.

`README.md`: pairing is done in Nickel or over SSH, **not** in koboy, and why
(spec §8.6: pairing needs a D-Bus agent, and koboy will not link libdbus).

`CLAUDE.md`: add the Bluetooth constraint to **What the hardware overruled** —
"Nickel owns the Bluetooth audio stack" was v1's stated reason for deferring
audio and it is false; `bluetoothd` and `bluealsa` are init children that
survive the takeover.

The spec: replace §8.5's three unknowns with the measured answers, and update
§8.2's unverified `rtk_hciattach` claim with what task 1 step 1 found.

- [ ] **Step 4: Commit**

```bash
git add TESTED.md README.md CLAUDE.md docs/
git commit -m "docs: what Bluetooth actually does on the one verified device

TESTED.md gains the gamepad and headphone models, the measured latency
and CPU cost, and a plain statement of what remains unmeasured.

CLAUDE.md records the overruled assumption: v1 deferred audio because
'Nickel owns the Bluetooth audio stack', and that is false. bluetoothd
and bluealsa are init children in dbus-daemon's process group and are
untouched by stopping Nickel.

The closure is still libm, libc and the loader. The device carries
libbluetooth, libasound, libdbus-1, libsbc and libglib; koboy links none
of them."
```

---

## Plan self-review

**Spec coverage.** §8.1 → task 1 and the device-facts section; §8.2 → task 1;
§8.3 → tasks 2 and 4 (the closure claim, verified in task 6 step 2); §8.4 →
tasks 2 and 3; §8.5 → task 4; §8.5.1 → task 5; §8.6 → task 6 step 3
(documented, deliberately not implemented). Appendix A's caveat about the audio
path being inspected-not-exercised is retired by task 4 step 7.

**Placeholder scan.** Tasks 2 step 3, 3 step 2 and 4 step 3 describe
implementations in prose with explicit required behaviours rather than complete
code, because each is a parser over a kernel or tool output format whose exact
bytes must be read from the device at implementation time — the test data in
each is real captured output and pins the contract. Every other step carries
literal code or literal commands.

**Type consistency.** `btinput_scan`, `btinput_is_gamepad` and
`btinput_parse_handlers` keep one signature across tasks 2 and 3.
`btaudio_open`/`btaudio_write`/`btaudio_close`/`btaudio_kill_async` are
consistent across tasks 4 and 5. `koboy_config.audio` is introduced once, in
task 4.

**Sequencing note.** Task 5 hardens task 4's child, so 4 must land first. Tasks
2 and 3 are independent of 4 and 5 and can be done in either order relative to
them; task 1 precedes all of them because everything needs `hci0`.
