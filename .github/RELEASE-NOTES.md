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

**The touchscreen release.** Everything in 0.5.3 comes out of [#1](https://github.com/dimitriuz/koboy/issues/1), filed by an owner whose Kobo Aura H2O showed the menu, accepted exactly one tap, and then went dead.

## What changed

koboy learned that a finger has left the panel by watching `ABS_MT_TRACKING_ID` go negative. That is what a Libra 2 sends, and it was the only thing the test suite had ever fed. **Three other families ship on the Kobo line and none of them does that:**

| Family | Devices | What it sends instead |
|---|---|---|
| Phoenix | Aura H2O, Aura, Aura SE r1, Glo HD, Touch 2.0, Nia, KA1 | keeps the tracking id through the lift |
| Snow / Mk7 | Clara HD, Forma, Aura H2O² r2 | repeats the id unchanged |
| pre-multitouch | Touch A/B/C, Mini, Glo, Aura HD | never sends one at all |

All of them announce the lift with `BTN_TOUCH`, which koboy was discarding as an unrecognised key. So the contact latched down on first touch and never came up — one tap in the menus and nothing afterwards, and in a game a joypad button held down for the rest of the session.

Two more problems came out of the same investigation, neither of which the reporter could reach because of the first:

- **Only one finger was tracked on those panels.** Phoenix separates two contacts in a way koboy was not reading, so the second landed on top of the first. No holding the d-pad while pressing A — not an annoyance, unplayability.
- **The ROM browser had no way out** on a Kobo with no page-turn buttons. `..` goes *up*, and the root has no up, so the only exits were picking a game or holding the power button. There is now a `<< MAIN MENU` row at the top of every folder.

## If you have one of those Kobos, this is the release to try

**None of it is tested on the hardware it is for.** Nobody working on koboy owns an affected device; the fixes are built against the event streams FBInk records for each family, which is the best evidence available and is not the same as a real capture. The device *can* run the build — every binary in the archive targets the ARMv7-A + NEON baseline every Kobo has had since the Touch, and needs nothing outside the base system — but whether the touch handling is right on your panel is exactly the thing that needs someone to try it.

A report either way is worth having. If it still misbehaves, `.adds/koboy/koboy.log` says what koboy decided about your screen and is the single most useful thing to attach.

## Installing

Unzip the koboy archive below at the root of the drive your Kobo shows up as over USB, put ROMs in `.adds/koboy/roms/`, and launch from NickelMenu.

**The archive looks almost empty until you extract it.** Everything lives in `.adds`, and the leading dot hides it from Linux file managers, Finder and Explorer — that is where Kobo add-ons go, and your extractor creates it for you. README.md has the rest, including the screen settings; try `MENU → MOTION` first, it is the one that stops moving sprites smearing.

One archive, ~18.6 MB. 13.6 MB of that is the arcade core — delete `.adds/koboy/fbneo_libretro.so` if you have no arcade romset.

The probe archive is the device profiler on its own, for characterising a Kobo nobody has tried without installing the emulator. See TESTED.md's "How to add a row".

---

Played and verified on a Kobo Libra 2 and nowhere else. Every core is built from source at the commit `scripts/pins.txt` records. koboy is GPL-3; see LICENSES.md for each core's own terms.
