# koboy-probe

A standalone device profiler for koboy (https://github.com/ -- see the main
project). It exists so someone with a Kobo the author does not own (Clara,
Sage, Elipsa, Libra Colour -- anything other than the Libra 2 in TESTED.md)
can characterise their device and contribute a row, without installing the
emulator itself first.

## Building just the probe

    export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
    make probe-dist        # -> dist/koboy-probe-0.1.0.zip

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

This is the default and safe mode: it never stops Nickel, never changes the
framebuffer's bit depth, and has nothing to restore afterwards. Run it while
reading, if you like -- the screen will flicker through test patterns for
under a minute while it times panel refreshes, and your book reappears once
it finishes. It writes:

```
/mnt/onboard/koboy-probe-<device>.txt
```

as `key=value` lines. Open that file (or just re-read the terminal output)
and paste the relevant lines into a new row of the main project's
`TESTED.md`: `device=`, `platform=`, `panel=`, `stride=`, `touch_slots=`,
`touch_transpose=`, and `wfm_fast_name=`/`wfm_fast_ms=` are the ones that
matter most.

**Check `unreliable_wait_for=` before trusting `wfm_fast_ms=`.** If it reads
`1`, the panel's "update complete" ioctl can time out instead of reporting
real completion, and every `*_block_us_*` figure in the file -- `wfm_fast_ms`
included -- may be measuring that stall rather than actual panel latency.
The probe repeats the same caveat as `wfm_fast_ms_caveat=` right next to the
number when this applies; `wfm_fast_submit_ms=` is unaffected and the more
trustworthy figure on such a device.

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

It then waits 15 seconds for you to press every physical button and touch
the screen a few times, and appends what it saw (key codes, touch contact
count, last raw coordinates) to the same output file. The touch count is a
rough check ("does this panel report multitouch at all"), not a real
multi-finger decode -- it has no per-slot tracking, so touching with two
fingers at once can over-count or blend the reported coordinates. Bring
Nickel back afterwards the way `scripts/koboy.sh` in the main project does,
or reboot -- a Nickel restarted by hand outside that script has been
observed to corrupt the device's own identity file, so a reboot is the safe
default if in doubt.

## Contributing a row

Open an issue or pull request against the main koboy repository with the
contents of `koboy-probe-<device>.txt` and a note on what worked and what
did not. "d-pad unusable, touch axes came out transposed" is worth more than
no report at all -- unverified is the default state for every device but
one, and this file is how that changes.
