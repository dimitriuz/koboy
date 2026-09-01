<!-- THE BODY OF THE NEXT GITHUB RELEASE. .github/workflows/release.yml passes
     this file to `gh release create --notes-file`, so whatever is here is what
     a downloader reads.

     REWRITE THE TOP HALF BEFORE YOU TAG. It is about ONE release and goes stale
     the moment that release ships; the "Installing" half below is boilerplate
     and can be left alone. The workflow's `check` job REFUSES to build a tag
     whose version this file does not mention, which is what stops the last
     release's notes going out attached to this one's archive -- the same class
     of mistake the tag-versus-VERSION gate already covers.

     Markdown, rendered by GitHub. This comment does not show. -->

**0.5.4 is for one person and anyone with the same Kobo.** 0.5.3 tried to fix
[#1](https://github.com/dimitriuz/koboy/issues/1) — a Kobo Aura H2O where the
menus took one tap and then went dead — and did not. This release is mostly
about being able to tell why.

## If tapping does not work on your Kobo

There are at least four ways a Kobo touchscreen can describe a finger, and koboy
has only ever been tested against the one the author's Libra 2 speaks. 0.5.3
guessed at the others from a description that turned out to be about a different
kind of panel entirely — the H2O uses an *infrared* touch frame, not the
capacitive type that guess was built for.

So this release stops guessing and starts asking your device:

- **koboy now logs what your panel can send**, every launch, no setup. Look for
  a line beginning `koboy: touch caps` in `.adds/koboy/koboy.log`.
- **`trace_touch = true`** in `.adds/koboy/koboy.ini` records the touch events
  themselves. Turn it on, tap around for a few seconds, and send the log with
  your report. It is noisy, so leave it off otherwise.

Between them those two turn "it doesn't work on my model" into something
fixable by someone who does not own your model. If tapping is broken for you,
that log is the single most useful thing you can send.

## Two fixes worth trying

- **A touchscreen that reported nothing at all.** koboy was opening, closing and
  reopening the touch node at startup; on the infrared panels used by the Aura
  H2O and Aura HD, open and close are power commands to the touch frame's own
  firmware, and FBInk documents that shuffle as able to leave the panel silent
  for the whole session. koboy no longer does it.
- **A finger that never came up.** Some panels report a lift by simply not
  mentioning the finger any more, rather than by sending anything about it.
  koboy was waiting for an event that was never coming.

Whether either one is *the* problem on the Aura H2O is unknown — nobody working
on koboy owns one. If 0.5.4 fixes it for you, please say so; if it does not, the
trace above is what settles it.

Everything else in 0.5.4 is release plumbing and changes nothing on the device.

## Installing

Unzip the koboy archive below at the root of the drive your Kobo shows up as over USB, put ROMs in `.adds/koboy/roms/`, and launch from NickelMenu.

**The archive looks almost empty until you extract it.** Everything lives in `.adds`, and the leading dot hides it from Linux file managers, Finder and Explorer — that is where Kobo add-ons go, and your extractor creates it for you. README.md has the rest, including the screen settings; try `MENU → MOTION` first, it is the one that stops moving sprites smearing.

One archive, ~18.6 MB. 13.6 MB of that is the arcade core — delete `.adds/koboy/fbneo_libretro.so` if you have no arcade romset.

The probe archive is the device profiler on its own, for characterising a Kobo nobody has tried without installing the emulator. See TESTED.md's "How to add a row".

---

Played and verified on a Kobo Libra 2 and nowhere else. Every core is built from source at the commit `scripts/pins.txt` records. koboy is GPL-3; see LICENSES.md for each core's own terms.
