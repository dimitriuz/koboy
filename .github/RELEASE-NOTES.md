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

**If tapping never worked on your Kobo, try 0.5.5.** This is the third attempt at
[#1](https://github.com/dimitriuz/koboy/issues/1) and the first one built on a
recording of the hardware rather than a guess about it.

## What was wrong

There are several ways a Kobo touchscreen can report a finger, and koboy
understood the one its author's device speaks. On a Kobo Aura H2O the menus took
exactly one tap and then went dead: koboy was waiting to be told the finger had
lifted, in a way that panel never says.

0.5.3 and 0.5.4 both guessed at what it *does* say, from a description that
turned out to be about a different kind of touchscreen. 0.5.4 added a way to
record the real thing instead, the owner sent one, and it settled the question
in a line: the panel reports a lift by saying the contact has **shrunk to
nothing** — and never sends the signal koboy had been waiting for, even though
it advertises that it can.

## What changed

koboy no longer decides in advance which signal to trust. It watches how each
one behaves on your particular panel and believes the ones that have shown they
work — a panel that reports real contact strength proves it the moment you touch
the screen, and a panel whose sensor is stuck at zero is never allowed to claim
your finger has gone. The device answers the question about itself, which is how
koboy already works out your screen size and touch orientation.

**This should fix tapping on the Aura H2O, and may fix it on the Aura, Aura HD,
Aura SE, Glo HD, Touch 2.0, Nia, Clara HD and Forma.** It cannot affect devices
where tapping already worked: the new handling runs only on panels that describe
touches the older way, and a Libra 2 executes none of it. That was checked on
the device, not assumed.

## If it still does not work

Open `.adds/koboy/koboy.ini`, set `trace_touch = true`, tap around for a few
seconds and send `.adds/koboy/koboy.log` with your report. That is what turned
this from two failed guesses into a fix, and it will do the same for the next
panel.

One thing still unknown even when tapping works: whether taps land exactly where
your finger is. Menu rows span the whole width so a sideways error does not show
up there, but it would ruin the on-screen d-pad. If the buttons feel wrong in a
game, the same trace answers that too.

## Installing

Unzip the koboy archive below at the root of the drive your Kobo shows up as over USB, put ROMs in `.adds/koboy/roms/`, and launch from NickelMenu.

**The archive looks almost empty until you extract it.** Everything lives in `.adds`, and the leading dot hides it from Linux file managers, Finder and Explorer — that is where Kobo add-ons go, and your extractor creates it for you. README.md has the rest, including the screen settings; try `MENU → MOTION` first, it is the one that stops moving sprites smearing.

One archive, ~18.6 MB. 13.6 MB of that is the arcade core — delete `.adds/koboy/fbneo_libretro.so` if you have no arcade romset.

The probe archive is the device profiler on its own, for characterising a Kobo nobody has tried without installing the emulator. See TESTED.md's "How to add a row".

---

Played and verified on a Kobo Libra 2; touch input additionally confirmed on a Kobo Aura H2O. README.md's "Will it run on my Kobo?" says where every other model stands. Every core is built from source at the commit `scripts/pins.txt` records. koboy is GPL-3; see LICENSES.md for each core's own terms.
