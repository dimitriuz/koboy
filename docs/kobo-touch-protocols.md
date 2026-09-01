# How a Kobo describes a finger

Three releases went into learning this the hard way, so it is written down once.
`src/input.c` implements it; this file is why.

## The licence line, first

**KOReader is AGPL-3.0 and koboy is GPL-3.** AGPL code cannot be copied into
this project without relicensing all of it, so nothing here is copied from
KOReader and nothing should be. What is taken is *facts about hardware* — which
panel sends which event — which are not anybody's expression. Its
`frontend/device/kobo/device.lua` and `frontend/device/input.lua` are cited the
way a datasheet is cited, and they are the best datasheet these panels have.

FBInk (MIT, and vendored here) is the other reference. Where the two agree,
believe them. Where they disagree, believe the device.

## Four ways to say "the finger is gone"

A Kobo touchscreen reports a contact in one of a few dialects. koboy has met
three of them and been broken by two.

| Dialect | Contacts addressed by | The LIFT is | Kobos |
|---|---|---|---|
| **protocol B** | `ABS_MT_SLOT` | `ABS_MT_TRACKING_ID == -1` | Libra 2, Clara BW/Colour era, most modern |
| **"Phoenix"** | `ABS_MT_TRACKING_ID`, used as a slot number and **never** -1 | **`ABS_MT_TOUCH_MAJOR == 0`** | Aura, Aura H2O, Aura ONE, Glo HD, Touch 2.0, Nia, Aura SE |
| **"Snow"** | a slot-like `ABS_MT_TRACKING_ID`, also never -1 | **`BTN_TOUCH == 0`** | Clara HD, Forma, Nova, Elipsa, Sage, H2O² r2 |
| **pre-multitouch** | nothing; one contact only | `BTN_TOUCH == 0` | Touch A/B/C, Mini, Glo, Aura HD |

Both middle rows are confirmed by KOReader's handlers, and Phoenix is confirmed
by a capture off the device in github issue #1. On Snow, KOReader's own comment
is the clearest statement of it: the tracking id "will *never* be set to -1 on
contact lift, which is why we instead have to rely on `EV_KEY:BTN_TOUCH:0`".

Roughly half the Kobo line is Phoenix or Snow, so protocol B is not the common
case — it is only the case koboy's author owns.

## Three traps, each of which cost a release

**A capability bitmap is a claim, not a promise.** The Aura H2O's node
advertises `BTN_TOUCH` and `ABS_PRESSURE` and sends neither. koboy's
`touch caps` log line reads them with `EVIOCGBIT`, which is worth having and is
not evidence about what arrives. Only `trace_touch = true` answers that.

**A per-model table can be wrong for a given unit.** KOReader ships one and
still had to add a runtime escape: newer revisions of Snow devices turned up
with ordinary protocol-B panels, so it now disables its own Snow quirks when it
sees a `-1` (their comment cites the MobileRead thread). Model is not hardware.
This is the strongest argument for koboy deriving what it can at runtime.

**`ABS_MT_TOUCH_MAJOR` is both the answer and a trap.** It is the only lift
signal a Phoenix panel sends, and it reads 0 with a finger *down* on early Mk7
hardware — FBInk excludes it outright for that reason. No fixed choice of field
is right for both, which is why `input.c` ARMS an axis by seeing it positive
once before it will accept a zero from it as a lift. The device settles the
question about itself.

## Where the taps land

Separate from the protocol, and independent of it. Two facts, agreed by both
references:

- **`touch_switch_xy` is true on every Kobo.** KOReader states it flatly;
  FBInk defaults `touchSwapAxes = true` for the platform.
- **`touch_mirrored_x` is true on most**, and both projects default it true and
  override per model. The Libra 2 (`Io`) is one of the overrides — it does NOT
  mirror. The Aura H2O does. A couple of recent models mirror Y instead.

koboy takes the mirrors from FBInk and derives the swap itself from the reported
axis maxima. The two agreed on both devices seen so far. `docs/FOLLOWUPS.md`
#116 records where that derivation could disagree with the tables, and why the
tables would probably be right.

A mirror error is invisible in a menu — list rows span the full width, so a tap
lands on the intended row whichever side it thinks it is — and ruins the
on-screen d-pad. So "the menus work" is not evidence the transform is correct.
Two deliberate taps at two named corners, traced, is.
