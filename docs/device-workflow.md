# Working on the device

Everything in this file was learned by getting it wrong first. The build and
the test suite are the easy half; putting a binary on a Kobo, running it, and
getting the device back in one piece is the half with the traps.

## The device under test

| | |
|---|---|
| Model | Kobo Libra 2 — FBInk id **388**, codename `Io`, platform **Mark 9** |
| Firmware | 4.38.23684, kernel 4.1.15 armv7l |
| CPU / RAM | single-core Cortex-A9 + NEON, 496 MB |
| **glibc** | **2.19** — this is the binding constraint on the whole project |
| Panel | 1264x1680 @ 300 dpi. 8bpp stride 1280 B, 32bpp stride 5120 B |
| Input | `event0` = `gpio-keys` (**shares the node with KEY_POWER**), `event1` = `Elan Touchscreen`, raw 1680x1264, `transpose=1` |
| Buttons | KEY_F23(193) / KEY_F24(194). This device's own calibration chose `key_a = 194` |

Preinstalled by NiLuJe's KoboStuff and relied on: `fbink`, `fbdepth`, `evtest`,
`evemu`. `fbink -e` dumps the device state the code reads.

## Getting a shell

**The device does not answer ping.** Do not look for it with a ping sweep —
that cost time in the 2026-08-26 session and found nothing. Find it by
probing port 22 directly instead, e.g. `nmap -p 22 <subnet>` or a shell loop
of `nc -zw1 <host> 22` over the LAN's address range; dropbear answers even
though ICMP echo does not.

The device runs dropbear on the LAN as `root`. The password is the stock Kobo
one — ask the user, or set `KOBO_PW`; it is deliberately not committed here.

**There is no `sshpass`, `expect`, or `paramiko` on the dev machine.** Driving
`ssh` through a pty with stdlib Python is the way in:

```python
import os, pty, select, time
def run(argv, pw, timeout=300):
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp(argv[0], argv); os._exit(127)
    out, sent = b"", False
    # read; when b"assword" appears, os.write(fd, pw + b"\n") once
```

Always pass these, or host-key and pubkey prompts will hang the pty:

```
-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
-o PreferredAuthentications=password -o PubkeyAuthentication=no
```

**The dev shell is fish.** `$VAR` holding a string of flags does **not**
word-split the way bash does — pass ssh options literally, one per argument, or
you get `keyword stricthostkeychecking extra arguments at end of line`.

## Deploying

```sh
export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make dist                       # -> dist/koboy-0.1.0.zip, contents under .adds/koboy/
# scp the zip, then on the device:
cd /mnt/onboard && unzip -o -q /tmp/koboy-*.zip && sync
```

Three things that bite:

1. **Back up `koboy.ini` first.** A redeploy overwrites it, and that file holds
   the user's `key_a`/`key_b` from first-run calibration. This device calibrated
   to `194/193`, the reverse of the shipped defaults — restoring the defaults
   silently swaps A and B. Copy it to `koboy.ini.prev` and carry the keys
   forward.
2. **`zip -D` omits empty directory entries**, so a `save_dir` that ships as an
   empty directory does not exist after unzip. `mkdir -p` it.
3. Set the exec bit on `koboy`, `koboy.sh`, `koboy-probe` and the core `.so`
   after unzipping if the archive's modes did not survive.

## Running it — launch from the device's own menu, never over ssh

`scripts/koboy.sh` refuses to run unless `PLATFORM`, `PRODUCT` and
`NICKEL_HOME` are all set, and exits 3 with a message drawn on the panel. This
is not defensive boilerplate. Nickel must be stopped for koboy to read a single
input event — it holds `EVIOCGRAB` on every node — and **restarting Nickel from
a process that did not inherit its environment rewrites
`/mnt/onboard/.kobo/version`**, replacing the real serial with a placeholder:

```
before  <real-serial>,4.1.15,4.38.23684,...,00000000-...-000000000388
after   11:22:33:44:55:66,4.1.15,4.38.23684,...,
```

That file is how FBInk identifies the device, so afterwards every FBInk-based
tool on the device — KOReader included — silently loses its per-device quirks
(`deviceName='Unknown!'`, `Mark ?`, `hasEclipseWfm=0`). Only a reboot repairs
it. This happened once, by hand, and the gate exists so it cannot happen twice.

The gate is trivially spoofable by exporting the three names, and the comment in
the script says so. Spoofing it gets a *partial* environment and can do exactly
the damage above. Launch from **NickelMenu** (`/mnt/onboard/.adds/nm/koboy`) or
KFMon, which spawn from inside Nickel and inherit the real environment.

A correct run ends like this, and needs no reboot:

```
restore: framebuffer back to 32bpp rota=1 (now 32bpp)
restore: starting hindenburg and nickel
restore: done
```

### A narrower, safe exception: `--frames` over ssh, without the takeover

The rule above is about the full launcher (`scripts/koboy.sh`, the input
grab, the Nickel stop/restart) and it stands. But `koboy --frames N` run
directly over ssh, bypassing `koboy.sh` entirely, is a genuinely safe way to
exercise the core, the SRAM path, and the video/panel pipeline **without**
stopping Nickel or touching the input grab — used in the 2026-08-26 session
to verify the save path and get real per-stage timing. Because Nickel is
never stopped, this does **not** exercise the takeover, the touch d-pad, the
in-game MENU, or the ROM browser's real touch input (`--ui-script` stands in
for touch there). It is a way to get partial device truth cheaply, not a
substitute for the NickelMenu playtest that exercises the rest. Confirm
device integrity afterwards the same way as any other session (compare
`/mnt/onboard/.kobo/version`, check `fbink -e` still reports the real device
identity).

## Diagnosing a run

`/mnt/onboard/.adds/koboy/koboy.log`, rotated at 256 KB. Append a marker before
handing the device back so the next run is unambiguous:

```sh
echo "=== deploy $(date '+%F %T'): <what changed> ===" >> koboy.log
# afterwards:  awk '/=== deploy /{f=1} f' koboy.log
```

koboy's own summary line is the useful one:

```
koboy: stopped, 2261 presented frames, 0 game-rect cleanups, 16 large-area full refreshes
```

## Profiling a device nobody has tried

`koboy-probe --coexist` is safe over ssh with Nickel running: it never reads an
input event, never grabs a node, never changes bit depth. It writes
`/mnt/onboard/koboy-probe-<device>.txt`. `--takeover` needs Nickel stopped and
**appends** to that same file, adding the key codes and touch samples. Build
just the probe with `make probe-dist`. `docs/probe-readme.md` has the detail.

## Measurement caveat

Treat every absolute refresh timing as order-of-magnitude. The same region and
waveform on this device has measured 31.2, 46.7 and 67.5 ms across three
sessions — a factor of 2.2. And the device reports `unreliable_wait_for=1`,
which applies to the very ioctl a *blocking* measurement waits on, so blocking
figures are suspect by construction. The main loop never waits for completion;
the non-blocking path is what it actually depends on. Ratios reproduced across
sessions where absolutes did not, which is why the design rests on ratios.
