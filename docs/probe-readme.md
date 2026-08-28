# koboy-probe

A standalone device profiler for koboy (https://github.com/ -- see the main
project). It exists so someone with a Kobo the author does not own (Clara,
Sage, Elipsa, Libra Colour -- anything other than the Libra 2 in TESTED.md)
can characterise their device and contribute a row, without installing the
emulator itself first.

## Building just the probe

    export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
    make probe-dist        # -> dist/koboy-probe-$(VERSION).zip

This builds the probe and nothing else. `make dist` also cross-builds gambatte,
which takes minutes and which you do not need in order to characterise a device.

## Install

Unzip at the root of the drive the Kobo shows up as over USB. That creates
`.adds/koboy/koboy-probe` and writes nothing else -- no `KoboRoot.tgz`,
nothing under `/usr`. Uninstalling is deleting `.adds/koboy/`.

## Running it

Over SSH (`ssh root@<device>`), or from a terminal emulator on the device
itself:

```sh
cd /mnt/onboard/.adds/koboy
./koboy-probe --coexist
```

The default and safe mode: it never stops Nickel, never changes bit depth, and
has nothing to restore. Run it while reading if you like -- the screen flickers
through test patterns for under a minute and your book reappears. It writes:

```
/mnt/onboard/koboy-probe-<device>.txt
```

as `key=value` lines. Open that file (or just re-read the terminal output)
and paste the relevant lines into a new row of the main project's
`TESTED.md`: `device=`, `platform=`, `panel=`, `stride=`, `touch_slots=`,
`touch_transpose=`, and `wfm_fast_name=`/`wfm_fast_ms=` are the ones that
matter most.

**Check `unreliable_wait_for=` before trusting `wfm_fast_ms=`.** At `1`, the
panel's "update complete" ioctl can time out instead of reporting completion,
and every `*_block_us_*` figure -- `wfm_fast_ms` included -- may be measuring
that stall rather than panel latency. The probe repeats the caveat as
`wfm_fast_ms_caveat=` next to the number; `wfm_fast_submit_ms=` is unaffected
and more trustworthy on such a device.

On the one device measured, `unreliable_wait_for=1` cost six of fifty sweep
cells outright: they came back at almost exactly 5,000,000 us, the ioctl's own
timeout. **Discard any `*_block_us_*` value near 5000000 rather than averaging
it in.** What survives is worth having -- the remaining cells fitted `fixed
term + 15 to 22 ns/px` across a 49x span in area, reproduced to 0.1%, with the
fixed terms landing on plausible whole numbers of ~11.8 ms panel frames.
Appendix E of the design spec has the method.

### `sustain_*` -- how fast the panel will ACCEPT work

Alongside the blocking sweep, the probe submits updates to one region back to
back with no wait at all and reports `sustain_<wfm>_<solid|checker>_<WxH>_`
`period_us`, `submit_us` and `fill_us`. This exists because a pacing constant
must not depend on the ioctl above.

**Read `period_us` together with `fill_us` or not at all.** The loop period is
`max(panel period, fill + submit)`, so a cell whose fill cost approaches its
period is measuring the probe process rather than the panel.

**On the reference device it measures the driver, not the panel, and that is
itself the finding.** The theory was that the EPDC's descriptor pool would fill
and submission would block at the panel's rate. It never blocked: a new
full-rect update was accepted every 6-13 ms against a ~153 ms completion.
**There is no back-pressure below the application on this hardware** -- the
driver takes work it cannot do and returns success. If a new device's
`submit_us` rises to meet its blocking figure, that device DOES throttle, which
is worth a `TESTED.md` row on its own.

`solid` flips the region black/white (100% of pixels transitioning, the worst
case); `checker` phase-shifts an 8 px checkerboard (about 50%, closer to what
a dithered scroll asks for).

### `--takeover`

```sh
./koboy-probe --takeover
```

Reads real button presses and touch points, which needs Nickel's input
grab out of the way first. It refuses to run -- and says why -- if Nickel is
still up:

```sh
killall -TERM nickel hindenburg sickel
sleep 3
./koboy-probe --takeover
```

It waits 15 seconds for you to press every physical button and touch the
screen a few times, then APPENDS what it saw (key codes, touch contact count,
last raw coordinates) to the same file. The touch count is a rough check
("does this panel report multitouch at all"), not a multi-finger decode -- it
has no per-slot tracking, so two fingers at once can over-count or blend the
coordinates. Bring Nickel back the way `scripts/koboy.sh` does, or REBOOT: a
Nickel restarted by hand outside that script has been observed to corrupt the
device's own identity file.

## Contributing a row

Open an issue or pull request with the contents of
`koboy-probe-<device>.txt` and a note on what worked and what did not. "d-pad
unusable, touch axes came out transposed" is worth more than no report --
unverified is the default state for every device but one, and this is how that
changes.
