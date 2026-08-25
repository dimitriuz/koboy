# koboy v2 Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give koboy a ROM browser, an in-game menu, save states, a DMG-faithful
faceplate and multi-rect dirty regions, closing 14 of the 16 deferred findings on
the way.

**Architecture:** `main.c` becomes a four-mode state machine over an extracted,
pure UI layer (`text.c`, `ui.c`) fed by `koboy_input_state` rather than events, so
every screen is host-testable. The browser and the menu are one list widget with
two content sources. Save states mirror `sram.c`'s all-or-nothing discipline
through a shared `safefile` module. Video gains a measured cost model that splits
one merged dirty box into several when that is cheaper.

**Tech Stack:** C99, libc/libm/libdl only. Tests are host binaries built by the
existing `tests/test_*.c` wildcard rule and run with `make test`. Golden images
are PGM via `tests/pgm.h`.

**Spec:** `docs/superpowers/specs/2026-08-25-koboy-v2-design.md`

**Companion plan:** `docs/superpowers/plans/2026-08-25-koboy-v2-bluetooth.md`
covers spec §8. It depends on nothing here and nothing here depends on it.

## Global Constraints

- **C99 only, no C++.** No dependency beyond libc, libm, libdl.
- **The shipped ARM binary's dependency closure must stay exactly** `libm.so.6`,
  `libc.so.6`, `ld-linux-armhf.so.3`. `scripts/verify-core.sh` enforces it.
- **glibc 2.19** is the device floor; the toolchain is Linaro 4.9-2014.09.
- **Never `#include <linux/input.h>` in portable code.** Use the project's own
  `koboy_ev` mirror and keycode constants.
- **Tests must pass on the dev host**, not only on-device. `make test` is the gate.
- **New `src/*.c` files are picked up automatically** by the Makefile's
  `SRC := $(filter-out src/main.c src/probe.c src/platform_%.c,$(wildcard src/*.c))`
  and linked into every test binary. No Makefile edit is needed for them.
- **New `tests/test_*.c` files are picked up automatically** by `TESTSRC := $(wildcard tests/test_*.c)`.
- **After writing any safety or regression test, break the thing it guards and
  confirm the test fails.** Record the mutant and its output in the commit body.
  A test that can only fail via undefined behaviour is not a test.
- **Comments record why, not what.** Clamps and guards carry a note saying they
  are live so nobody deletes them as dead code.
- **ROMs are git-ignored** (`*.gb`, `*.gbc`, `*.sav`) and must never be committed.

---

### Task 1: Per-stage timing statistics

Closes nothing on its own; produces the numbers Task 12's cost model is tuned
against, and the run that closes follow-up #3.

**Files:**
- Create: `src/stats.h`
- Create: `src/stats.c`
- Create: `tests/test_stats.c`
- Modify: `src/main.c` (the emulator loop and the exit summary)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum { KOBOY_STAGE_CORE, KOBOY_STAGE_SUBMIT, KOBOY_STAGE_BLIT, KOBOY_STAGE_REFRESH, KOBOY_STAGE_COUNT }`
  - `void stats_reset(koboy_stats *s)`
  - `void stats_add(koboy_stats *s, int stage, uint64_t us)`
  - `uint64_t stats_mean_us(const koboy_stats *s, int stage)`
  - `uint64_t stats_max_us(const koboy_stats *s, int stage)`
  - `void stats_format(const koboy_stats *s, char *out, size_t n)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_stats.c`:

```c
#include "test.h"
#include "stats.h"

TEST_MAIN({
    koboy_stats s;
    stats_reset(&s);

    /* An empty stage must report zero rather than divide by zero. */
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 0);
    CHECK_EQ_INT(stats_max_us(&s, KOBOY_STAGE_CORE), 0);

    stats_add(&s, KOBOY_STAGE_CORE, 10);
    stats_add(&s, KOBOY_STAGE_CORE, 20);
    stats_add(&s, KOBOY_STAGE_CORE, 60);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 30);
    CHECK_EQ_INT(stats_max_us(&s, KOBOY_STAGE_CORE), 60);

    /* Stages are independent. */
    stats_add(&s, KOBOY_STAGE_REFRESH, 1000);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 30);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_REFRESH), 1000);

    /* Out-of-range stage indices are ignored, not written through. A bad
       index here would corrupt the adjacent stage's totals, which is the
       kind of bug that shows up as a nonsense number in a bug report. */
    stats_add(&s, -1, 999999);
    stats_add(&s, KOBOY_STAGE_COUNT, 999999);
    CHECK_EQ_INT(stats_mean_us(&s, KOBOY_STAGE_CORE), 30);

    char buf[256];
    stats_format(&s, buf, sizeof buf);
    CHECK(strstr(buf, "core=") != NULL);
    CHECK(strstr(buf, "refresh=") != NULL);

    /* Must not overrun a short buffer. */
    char tiny[8];
    stats_format(&s, tiny, sizeof tiny);
    CHECK_EQ_INT(strlen(tiny) < sizeof tiny, 1);
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_stats`
Expected: FAIL — `fatal error: stats.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `src/stats.h`:

```c
#ifndef KOBOY_STATS_H
#define KOBOY_STATS_H
#include <stddef.h>
#include <stdint.h>

/* Per-stage timing for one run of the emulator loop. Exists because every
   absolute figure this project has measured moved by up to a factor of 2.2
   between sessions, so the multi-rect cost model in video.c is tuned against
   numbers from the actual device rather than from the design spec. */
enum {
    KOBOY_STAGE_CORE = 0,   /* retro_run for one emulated frame */
    KOBOY_STAGE_SUBMIT,     /* video_submit: convert, scale, quantise, diff */
    KOBOY_STAGE_BLIT,       /* platform blit_gray8 */
    KOBOY_STAGE_REFRESH,    /* platform refresh submission (non-blocking) */
    KOBOY_STAGE_COUNT
};

typedef struct {
    uint64_t      total_us[KOBOY_STAGE_COUNT];
    uint64_t      max_us[KOBOY_STAGE_COUNT];
    unsigned long count[KOBOY_STAGE_COUNT];
} koboy_stats;

void     stats_reset(koboy_stats *s);
void     stats_add(koboy_stats *s, int stage, uint64_t us);
uint64_t stats_mean_us(const koboy_stats *s, int stage);
uint64_t stats_max_us(const koboy_stats *s, int stage);

/* Writes one line, always NUL-terminated, never exceeding n bytes. */
void     stats_format(const koboy_stats *s, char *out, size_t n);
#endif
```

- [ ] **Step 4: Write the implementation**

Create `src/stats.c`:

```c
#include "stats.h"
#include <stdio.h>
#include <string.h>

/* Live bounds check, not dead code: main.c passes a stage index derived from
   control flow, and an out-of-range write here would silently corrupt the
   neighbouring stage's totals -- producing a plausible-looking wrong number in
   a bug report, which is worse than a crash. */
static int valid(int stage) { return stage >= 0 && stage < KOBOY_STAGE_COUNT; }

void stats_reset(koboy_stats *s) { memset(s, 0, sizeof *s); }

void stats_add(koboy_stats *s, int stage, uint64_t us)
{
    if (!valid(stage)) return;
    s->total_us[stage] += us;
    s->count[stage]++;
    if (us > s->max_us[stage]) s->max_us[stage] = us;
}

uint64_t stats_mean_us(const koboy_stats *s, int stage)
{
    if (!valid(stage) || s->count[stage] == 0) return 0;
    return s->total_us[stage] / s->count[stage];
}

uint64_t stats_max_us(const koboy_stats *s, int stage)
{
    return valid(stage) ? s->max_us[stage] : 0;
}

void stats_format(const koboy_stats *s, char *out, size_t n)
{
    /* snprintf truncates rather than overruns, and always terminates for
       n >= 1. The n == 0 guard is required because snprintf may not write at
       all in that case, leaving `out` untouched and unterminated. */
    if (n == 0) return;
    snprintf(out, n,
             "core=%luus/%luus submit=%luus/%luus blit=%luus/%luus "
             "refresh=%luus/%luus (mean/max)",
             (unsigned long)stats_mean_us(s, KOBOY_STAGE_CORE),
             (unsigned long)stats_max_us(s, KOBOY_STAGE_CORE),
             (unsigned long)stats_mean_us(s, KOBOY_STAGE_SUBMIT),
             (unsigned long)stats_max_us(s, KOBOY_STAGE_SUBMIT),
             (unsigned long)stats_mean_us(s, KOBOY_STAGE_BLIT),
             (unsigned long)stats_max_us(s, KOBOY_STAGE_BLIT),
             (unsigned long)stats_mean_us(s, KOBOY_STAGE_REFRESH),
             (unsigned long)stats_max_us(s, KOBOY_STAGE_REFRESH));
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/test_stats && ./build/test_stats`
Expected: PASS — `tests/test_stats.c: 12 checks, 0 failures`

- [ ] **Step 6: Verify the bounds guard is real (mutant)**

Change `valid()` to `return 1;`, rebuild, run. Expected: the two
`stats_add(&s, -1, ...)` / `KOBOY_STAGE_COUNT` checks make
`stats_mean_us(&s, KOBOY_STAGE_CORE)` wrong, and the test FAILS. Revert the
mutant. Record the output in the commit body.

- [ ] **Step 7: Wire the counters into the emulator loop**

In `src/main.c`, add `#include "stats.h"` beside the other project includes.
Inside `main()`, immediately after `pacer_init(&pace, ...)`:

```c
    koboy_stats stats;
    stats_reset(&stats);
```

Then wrap each stage. Replace the existing `core_run_frame(core);` line with:

```c
        uint64_t t0 = pf->now_us(pf->ctx);
        core_run_frame(core);
        stats_add(&stats, KOBOY_STAGE_CORE, pf->now_us(pf->ctx) - t0);
```

Replace the `koboy_rect r = video_submit(...)` statement with:

```c
        t0 = pf->now_us(pf->ctx);
        koboy_rect r = video_submit(vid, g_frame, (int)g_fw, (int)g_fh,
                                    g_fpitch, core_pixfmt(core));
        stats_add(&stats, KOBOY_STAGE_SUBMIT, pf->now_us(pf->ctx) - t0);
```

Wrap the blit:

```c
        t0 = pf->now_us(pf->ctx);
        pf->blit_gray8(pf->ctx, video_buffer(vid) + (size_t)r.y * video_stride(vid) + r.x,
                       r.w, r.h, video_stride(vid),
                       prof.game_x + r.x, prof.game_y + r.y);
        stats_add(&stats, KOBOY_STAGE_BLIT, pf->now_us(pf->ctx) - t0);
```

And the per-frame refresh (the one guarded by `mode`, not the cleanup):

```c
        t0 = pf->now_us(pf->ctx);
        pf->refresh(pf->ctx, prof.game_x + r.x, prof.game_y + r.y, r.w, r.h, mode);
        stats_add(&stats, KOBOY_STAGE_REFRESH, pf->now_us(pf->ctx) - t0);
```

- [ ] **Step 8: Print the summary at exit**

In `src/main.c`, immediately after the existing `say("koboy: %s, %lu presented frames, ...")`
call and before `printf("presented=%lu\n", presented);`:

```c
    /* Always printed, even under --quiet, for the same reason presented= is:
       this is the run's evidence, and a run whose numbers were suppressed is a
       run that has to be done again. */
    {
        char line[256];
        stats_format(&stats, line, sizeof line);
        fprintf(stderr, "koboy: stages %s\n", line);
    }
```

- [ ] **Step 9: Verify the whole suite and a host run**

Run: `make test`
Expected: every binary reports `0 failures`.

Run: `make host && ./build/koboy --core build/stub_core.so --rom /dev/null --frames 120 --panel 1264x1680 2>&1 | grep stages`
Expected: a line containing `core=`, `submit=`, `blit=`, `refresh=`.

- [ ] **Step 10: Commit**

```bash
git add src/stats.h src/stats.c tests/test_stats.c src/main.c
git commit -m "feat: per-stage timing counters for the emulator loop

Every absolute timing figure this project has measured moved by up to a
factor of 2.2 between sessions, so task 12's multi-rect cost model is
tuned against numbers from the device rather than from the spec. This is
where those numbers come from.

Mutant: with valid() forced to return 1, the out-of-range stats_add calls
corrupt KOBOY_STAGE_CORE's totals and the test fails."
```

- [ ] **Step 11: ON DEVICE — the baseline run that closes follow-up #3**

This is the run the plan exists to enable, and the first time `sram_load` and
`sram_save` execute on hardware. Requires the Kobo awake and on WiFi, and
`Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2).gb` — cartridge
type `0x03` (MBC1+RAM+BATTERY), RAM size `0x02` (8 KiB), so `core_sram()`
returns a live pointer where both v1 test titles returned NULL.

Follow `docs/device-workflow.md` for deployment. Then, in one session:

1. Launch from NickelMenu, start a new game, save in-game at the first
   opportunity Link's Awakening offers, quit via the power button.
2. Confirm `<save_dir>/Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2).srm`
   exists and is 8192 bytes.
3. Relaunch and confirm the save is offered and loads.
4. Retrieve `.adds/koboy/koboy.log` over USB and record the `stages` line.

Record the result in `TESTED.md` under the existing device row. If the save does
**not** survive, stop and fix `sram_load`/`sram_save` before any later task —
follow-up #3 exists precisely because that code changed materially in v1's final
fix round and has never run.

---

### Task 2: Extract `text.c` and grow the font

Closes nothing directly; unblocks Tasks 3, 7, 10 and 11, all of which need
labels. Today's table is A–Z and space, which renders a filename as
`ZELDA  USA EUROPE  REV  GB`.

**Files:**
- Create: `src/text.h`
- Create: `src/text.c`
- Create: `tests/test_text.c`
- Modify: `src/main.c` (delete `FONT5x7`, `glyph`, `draw_text`, `draw_centred`; call `text_*` instead)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `#define TEXT_GLYPH_W 5`, `TEXT_GLYPH_H 7`, `TEXT_ADVANCE 6`
  - `int text_measure(const char *s, int px)`
  - `void text_draw(uint8_t *fb, int stride, int W, int H, int x, int y, const char *s, int px, uint8_t ink)`
  - `void text_draw_centred(uint8_t *fb, int stride, int W, int H, int y, const char *s, int px, uint8_t ink)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_text.c`:

```c
#include "test.h"
#include "text.h"

/* Counts ink pixels, which is enough to distinguish "drew a glyph" from
   "drew nothing" without pinning down the exact bitmap. */
static int ink_count(const uint8_t *fb, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) if (fb[i] == 0x00) c++;
    return c;
}

TEST_MAIN({
    enum { W = 200, H = 40 };
    static uint8_t fb[W * H];

    CHECK_EQ_INT(text_measure("", 1), 0);
    CHECK_EQ_INT(text_measure("A", 1), TEXT_ADVANCE);
    CHECK_EQ_INT(text_measure("ABC", 2), 3 * TEXT_ADVANCE * 2);

    /* Digits must render. This is the regression the extraction exists for:
       the old table was A-Z plus space, so every digit came out blank and a
       ROM filename lost its numbers silently. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "0123456789", 1, 0x00);
    int digits = ink_count(fb, sizeof fb);
    CHECK(digits > 0);

    /* Punctuation a filename actually contains. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, ".,:-_/()'", 1, 0x00);
    CHECK(ink_count(fb, sizeof fb) > 0);

    /* Lowercase folds to uppercase rather than vanishing. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "abc", 1, 0x00);
    int lower = ink_count(fb, sizeof fb);
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "ABC", 1, 0x00);
    CHECK_EQ_INT(lower, ink_count(fb, sizeof fb));

    /* An unknown character renders as blank space, never as garbage and never
       out of bounds. */
    memset(fb, 0xFF, sizeof fb);
    text_draw(fb, W, W, H, 0, 0, "\x01\x7F", 1, 0x00);
    CHECK_EQ_INT(ink_count(fb, sizeof fb), 0);

    /* CLIPPING IS LIVE. Drawing past every edge must touch nothing outside the
       buffer. A guard band on both sides catches an unclamped write; this is
       checked by assertion on the band rather than by hoping a stray write
       lands somewhere observable. */
    static uint8_t guarded[16 + W * H + 16];
    memset(guarded, 0x5A, sizeof guarded);
    uint8_t *inner = guarded + 16;
    memset(inner, 0xFF, (size_t)W * H);
    text_draw(inner, W, W, H, -50, -50, "CLIP", 3, 0x00);
    text_draw(inner, W, W, H, W - 2, H - 2, "CLIP", 3, 0x00);
    text_draw(inner, W, W, H, 0, H + 5, "CLIP", 3, 0x00);
    int guard_ok = 1;
    for (int i = 0; i < 16; i++) if (guarded[i] != 0x5A) guard_ok = 0;
    for (int i = 0; i < 16; i++) if (guarded[16 + W * H + i] != 0x5A) guard_ok = 0;
    CHECK_EQ_INT(guard_ok, 1);

    /* Centring puts equal-ish margins either side. */
    memset(fb, 0xFF, sizeof fb);
    text_draw_centred(fb, W, W, H, 0, "AB", 1, 0x00);
    int first = -1, last = -1;
    for (int x = 0; x < W; x++)
        for (int y = 0; y < H; y++)
            if (fb[y * W + x] == 0x00) { if (first < 0) first = x; last = x; }
    CHECK(first > 0);
    CHECK(W - 1 - last > 0);
    CHECK(first - (W - 1 - last) <= TEXT_ADVANCE);
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_text`
Expected: FAIL — `fatal error: text.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `src/text.h`:

```c
#ifndef KOBOY_TEXT_H
#define KOBOY_TEXT_H
#include <stdint.h>

/* A 5x7 bitmap font, one byte per column, bit 0 = top row.
   Lifted out of main.c, where it existed for two calibration prompts, because
   v2 has three screens that render arbitrary strings -- and the old table was
   A-Z plus space, so a ROM filename lost every digit without saying so.
   Pulling in a font library for this would still be absurd. */

#define TEXT_GLYPH_W 5
#define TEXT_GLYPH_H 7
#define TEXT_ADVANCE 6          /* 5 columns plus one blank */

/* Width in panel pixels of `s` rendered at scale `px`. */
int  text_measure(const char *s, int px);

/* Draws `s` with its top-left at (x, y), clipped to the W x H buffer.
   Characters outside the table render as blank space. */
void text_draw(uint8_t *fb, int stride, int W, int H, int x, int y,
               const char *s, int px, uint8_t ink);

void text_draw_centred(uint8_t *fb, int stride, int W, int H, int y,
                       const char *s, int px, uint8_t ink);
#endif
```

- [ ] **Step 4: Write the implementation**

Create `src/text.c`:

```c
#include "text.h"
#include <string.h>

static const uint8_t ALPHA[26][5] = {
    { 0x7E,0x11,0x11,0x11,0x7E }, /* A */
    { 0x7F,0x49,0x49,0x49,0x36 }, { 0x3E,0x41,0x41,0x41,0x22 },
    { 0x7F,0x41,0x41,0x22,0x1C }, { 0x7F,0x49,0x49,0x49,0x41 },
    { 0x7F,0x09,0x09,0x01,0x01 }, { 0x3E,0x41,0x49,0x49,0x7A },
    { 0x7F,0x08,0x08,0x08,0x7F }, { 0x00,0x41,0x7F,0x41,0x00 },
    { 0x20,0x40,0x41,0x3F,0x01 }, { 0x7F,0x08,0x14,0x22,0x41 },
    { 0x7F,0x40,0x40,0x40,0x40 }, { 0x7F,0x02,0x04,0x02,0x7F },
    { 0x7F,0x04,0x08,0x10,0x7F }, { 0x3E,0x41,0x41,0x41,0x3E },
    { 0x7F,0x09,0x09,0x09,0x06 }, { 0x3E,0x41,0x51,0x21,0x5E },
    { 0x7F,0x09,0x19,0x29,0x46 }, { 0x46,0x49,0x49,0x49,0x31 },
    { 0x01,0x01,0x7F,0x01,0x01 }, { 0x3F,0x40,0x40,0x40,0x3F },
    { 0x1F,0x20,0x40,0x20,0x1F }, { 0x7F,0x20,0x18,0x20,0x7F },
    { 0x63,0x14,0x08,0x14,0x63 }, { 0x03,0x04,0x78,0x04,0x03 },
    { 0x61,0x51,0x49,0x45,0x43 }, /* Z */
};

static const uint8_t DIGIT[10][5] = {
    { 0x3E,0x51,0x49,0x45,0x3E }, /* 0 */
    { 0x00,0x42,0x7F,0x40,0x00 }, { 0x42,0x61,0x51,0x49,0x46 },
    { 0x21,0x41,0x45,0x4B,0x31 }, { 0x18,0x14,0x12,0x7F,0x10 },
    { 0x27,0x45,0x45,0x45,0x39 }, { 0x3C,0x4A,0x49,0x49,0x30 },
    { 0x01,0x71,0x09,0x05,0x03 }, { 0x36,0x49,0x49,0x49,0x36 },
    { 0x06,0x49,0x49,0x29,0x1E }, /* 9 */
};

/* PUNCT_CHARS[i] is drawn by PUNCT[i]. Kept as parallel arrays with one
   lookup so adding a character is one edit in each, and a mismatch in length
   is caught by the test's punctuation check rather than by silence. */
static const char PUNCT_CHARS[] = " .,:;-_/\\()[]'\"!?+*=%#&<>@";
static const uint8_t PUNCT[][5] = {
    { 0x00,0x00,0x00,0x00,0x00 }, /* space */
    { 0x00,0x00,0x40,0x00,0x00 }, /* .  */
    { 0x00,0x00,0xC0,0x00,0x00 }, /* ,  */
    { 0x00,0x00,0x24,0x00,0x00 }, /* :  */
    { 0x00,0x00,0xA4,0x00,0x00 }, /* ;  */
    { 0x08,0x08,0x08,0x08,0x08 }, /* -  */
    { 0x40,0x40,0x40,0x40,0x40 }, /* _  */
    { 0x20,0x10,0x08,0x04,0x02 }, /* /  */
    { 0x02,0x04,0x08,0x10,0x20 }, /* \  */
    { 0x00,0x1C,0x22,0x41,0x00 }, /* (  */
    { 0x00,0x41,0x22,0x1C,0x00 }, /* )  */
    { 0x00,0x7F,0x41,0x41,0x00 }, /* [  */
    { 0x00,0x41,0x41,0x7F,0x00 }, /* ]  */
    { 0x00,0x00,0x03,0x00,0x00 }, /* '  */
    { 0x00,0x03,0x00,0x03,0x00 }, /* "  */
    { 0x00,0x00,0x5F,0x00,0x00 }, /* !  */
    { 0x02,0x01,0x51,0x09,0x06 }, /* ?  */
    { 0x08,0x08,0x3E,0x08,0x08 }, /* +  */
    { 0x14,0x08,0x3E,0x08,0x14 }, /* *  */
    { 0x14,0x14,0x14,0x14,0x14 }, /* =  */
    { 0x23,0x13,0x08,0x64,0x62 }, /* %  */
    { 0x14,0x7F,0x14,0x7F,0x14 }, /* #  */
    { 0x36,0x49,0x55,0x22,0x50 }, /* &  */
    { 0x08,0x14,0x22,0x41,0x00 }, /* <  */
    { 0x00,0x41,0x22,0x14,0x08 }, /* >  */
    { 0x3E,0x41,0x5D,0x55,0x1E }, /* @  */
};

static const uint8_t BLANK[5] = { 0, 0, 0, 0, 0 };

static const uint8_t *glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return ALPHA[ch - 'A'];
    if (ch >= '0' && ch <= '9') return DIGIT[ch - '0'];
    for (int i = 0; PUNCT_CHARS[i]; i++)
        if (PUNCT_CHARS[i] == ch) return PUNCT[i];
    return BLANK;             /* unknown renders as space, never as garbage */
}

int text_measure(const char *s, int px)
{
    if (px < 1) px = 1;
    return (int)strlen(s) * TEXT_ADVANCE * px;
}

void text_draw(uint8_t *fb, int stride, int W, int H, int x, int y,
               const char *s, int px, uint8_t ink)
{
    if (px < 1) px = 1;
    for (const char *p = s; *p; p++, x += TEXT_ADVANCE * px) {
        const uint8_t *g = glyph(*p);
        for (int col = 0; col < TEXT_GLYPH_W; col++) {
            for (int row = 0; row < TEXT_GLYPH_H; row++) {
                if (!(g[col] & (1u << row))) continue;
                for (int dy = 0; dy < px; dy++) {
                    int fy = y + row * px + dy;
                    /* Live clamps. text_draw is called with coordinates
                       derived from panel geometry and from string lengths the
                       caller does not bound, so both edges are reachable --
                       the ROM browser renders filenames of any length. */
                    if (fy < 0 || fy >= H) continue;
                    for (int dx = 0; dx < px; dx++) {
                        int fx = x + col * px + dx;
                        if (fx >= 0 && fx < W)
                            fb[(size_t)fy * stride + fx] = ink;
                    }
                }
            }
        }
    }
}

void text_draw_centred(uint8_t *fb, int stride, int W, int H, int y,
                       const char *s, int px, uint8_t ink)
{
    text_draw(fb, stride, W, H, (W - text_measure(s, px)) / 2, y, s, px, ink);
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/test_text && ./build/test_text`
Expected: PASS — `tests/test_text.c: 13 checks, 0 failures`

- [ ] **Step 6: Verify the clipping guard is real (mutant)**

Delete the `if (fy < 0 || fy >= H) continue;` line, rebuild, run.
Expected: the guard-band check FAILS. Revert. Record the output.

- [ ] **Step 7: Delete the duplicate font from `main.c`**

In `src/main.c`:
- Add `#include "text.h"` beside the other project includes.
- Delete the `FONT5x7` table, the `glyph()` function, `draw_text()` and
  `draw_centred()` entirely, along with the comment block introducing them.
- Replace the two `draw_centred(...)` call sites in the calibration loop with
  `text_draw_centred(...)` — the parameter lists are identical.

- [ ] **Step 8: Verify nothing regressed**

Run: `make test && make host`
Expected: all tests pass, host binary links.

Run: `bash tests/smoke_host.sh`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add src/text.h src/text.c tests/test_text.c src/main.c
git commit -m "refactor: extract text.c and give the font digits and punctuation

main.c carried a 5x7 font for two calibration prompts. v2 has three
screens that render arbitrary strings, and the old table was A-Z plus
space -- so a ROM filename lost every digit and every separator silently.

Mutant: removing the vertical clip in text_draw makes the guard-band
assertion fail."
```

---

### Task 3: The list widget

**Files:**
- Create: `src/ui.h`
- Create: `src/ui.c`
- Create: `tests/test_ui.c`

**Interfaces:**
- Consumes: `text_draw`, `text_draw_centred`, `text_measure` (Task 2);
  `koboy_input_state`, `koboy_rect`, `KOBOY_BTN_A`, `KOBOY_BTN_B`,
  `KOBOY_MAX_TOUCH` (`src/koboy.h`).
- Produces:
  - `typedef enum { UI_NONE = 0, UI_SELECT, UI_PAGE_NEXT, UI_PAGE_PREV } ui_action;`
  - `void ui_list_init(koboy_ui_list *u, const char *title, const char *const *items, int count, int x, int y, int w, int h)`
  - `int ui_list_rows(const koboy_ui_list *u)`
  - `int ui_list_pages(const koboy_ui_list *u)`
  - `void ui_list_render(const koboy_ui_list *u, uint8_t *fb, int stride, int W, int H)`
  - `ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st, int *out_index)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_ui.c`:

```c
#include "test.h"
#include "ui.h"
#include <string.h>

static koboy_input_state touch_at(int x, int y)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    st.touch[0].x = x; st.touch[0].y = y; st.touch[0].down = true;
    return st;
}

static koboy_input_state released(void)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    return st;
}

static koboy_input_state button(uint16_t bits)
{
    koboy_input_state st;
    memset(&st, 0, sizeof st);
    st.buttons = bits;
    return st;
}

/* Row centre in panel coordinates for row `r` of the current page. */
static int row_y(const koboy_ui_list *u, int r)
{
    return u->y + u->row_h + u->row_h * r + u->row_h / 2;
}

TEST_MAIN({
    static const char *const items[] = {
        "ZELDA.GB", "TETRIS.GB", "KIRBY 2.GBC", "DUCK.GB", "POKEMON.GBC",
        "SIX.GB", "SEVEN.GB", "EIGHT.GB", "NINE.GB", "TEN.GB",
        "ELEVEN.GB", "TWELVE.GB", "THIRTEEN.GB",
    };
    const int N = (int)(sizeof items / sizeof items[0]);

    koboy_ui_list u;
    ui_list_init(&u, "CHOOSE A GAME", items, N, 100, 200, 800, 900);

    CHECK(ui_list_rows(&u) > 0);
    CHECK(ui_list_pages(&u) >= 1);

    int idx = -1;

    /* A touch that is merely HELD produces one action, not one per poll.
       Level-triggered would select an item sixty times a second. */
    koboy_input_state down = touch_at(u.x + u.w / 2, row_y(&u, 0));
    CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_SELECT);
    CHECK_EQ_INT(idx, 0);
    CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_NONE);
    CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_NONE);

    /* Release, then a second tap on row 2 selects index 2. */
    koboy_input_state up = released();
    CHECK_EQ_INT(ui_list_feed(&u, &up, &idx), UI_NONE);
    koboy_input_state down2 = touch_at(u.x + u.w / 2, row_y(&u, 2));
    CHECK_EQ_INT(ui_list_feed(&u, &down2, &idx), UI_SELECT);
    CHECK_EQ_INT(idx, 2);
    CHECK_EQ_INT(ui_list_feed(&u, &up, &idx), UI_NONE);

    /* Paging with the page-turn buttons, also edge-triggered. */
    if (ui_list_pages(&u) > 1) {
        koboy_input_state b = button(KOBOY_BTN_B);
        CHECK_EQ_INT(ui_list_feed(&u, &b, &idx), UI_PAGE_NEXT);
        CHECK_EQ_INT(u.page, 1);
        CHECK_EQ_INT(ui_list_feed(&u, &b, &idx), UI_NONE);   /* held, not repeated */
        koboy_input_state none = released();
        CHECK_EQ_INT(ui_list_feed(&u, &none, &idx), UI_NONE);

        /* A selection on page 1 must index into the SECOND page, not the
           first. Getting this wrong loads the wrong ROM, which is the whole
           point of the widget. */
        int rows = ui_list_rows(&u);
        koboy_input_state d = touch_at(u.x + u.w / 2, row_y(&u, 0));
        CHECK_EQ_INT(ui_list_feed(&u, &d, &idx), UI_SELECT);
        CHECK_EQ_INT(idx, rows);
        CHECK_EQ_INT(ui_list_feed(&u, &up, &idx), UI_NONE);

        koboy_input_state a = button(KOBOY_BTN_A);
        CHECK_EQ_INT(ui_list_feed(&u, &a, &idx), UI_PAGE_PREV);
        CHECK_EQ_INT(u.page, 0);
    }

    /* Paging never runs off either end. */
    koboy_ui_list s;
    static const char *const one[] = { "ONLY.GB" };
    ui_list_init(&s, "ONE", one, 1, 0, 0, 400, 400);
    koboy_input_state nb = released(), bb = button(KOBOY_BTN_B);
    CHECK_EQ_INT(ui_list_feed(&s, &bb, &idx), UI_NONE);
    CHECK_EQ_INT(s.page, 0);
    CHECK_EQ_INT(ui_list_feed(&s, &nb, &idx), UI_NONE);
    koboy_input_state ab = button(KOBOY_BTN_A);
    CHECK_EQ_INT(ui_list_feed(&s, &ab, &idx), UI_NONE);
    CHECK_EQ_INT(s.page, 0);

    /* A tap on the last page must never select past the end of the list.
       The last page is short, so the rows below the final item are dead. */
    koboy_ui_list t;
    ui_list_init(&t, "SHORT", items, N, 0, 0, 400, 400);
    while (t.page + 1 < ui_list_pages(&t)) {
        koboy_input_state b = button(KOBOY_BTN_B), r = released();
        ui_list_feed(&t, &b, &idx);
        ui_list_feed(&t, &r, &idx);
    }
    int last_rows = ui_list_rows(&t);
    for (int r = 0; r < last_rows; r++) {
        koboy_input_state d = touch_at(t.x + t.w / 2, row_y(&t, r));
        int got = -1;
        ui_action a2 = ui_list_feed(&t, &d, &got);
        koboy_input_state rel = released();
        ui_list_feed(&t, &rel, &got);
        if (a2 == UI_SELECT) CHECK(got >= 0 && got < N);
    }

    /* A touch outside the list region selects nothing. */
    koboy_ui_list o;
    ui_list_init(&o, "OUT", items, N, 100, 200, 800, 900);
    koboy_input_state far = touch_at(10, 10);
    CHECK_EQ_INT(ui_list_feed(&o, &far, &idx), UI_NONE);

    /* Rendering is clipped and draws something. */
    enum { W = 1264, H = 1680 };
    static uint8_t fb[W * H];
    memset(fb, 0xFF, sizeof fb);
    ui_list_render(&u, fb, W, W, H);
    int painted = 0;
    for (size_t i = 0; i < sizeof fb; i++) if (fb[i] != 0xFF) painted++;
    CHECK(painted > 0);
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_ui`
Expected: FAIL — `fatal error: ui.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `src/ui.h`:

```c
#ifndef KOBOY_UI_H
#define KOBOY_UI_H
#include "koboy.h"

/* One list widget, used for BOTH the ROM browser and the in-game menu.
   They are the same thing -- a titled list of strings you tap, with paging --
   and writing them separately would be duplication wearing a disguise.

   It consumes koboy_input_state rather than evdev events, which is what makes
   it a pure host unit test rather than device theatre, and it renders into the
   caller's panel buffer without ever touching the platform. */

typedef enum { UI_NONE = 0, UI_SELECT, UI_PAGE_NEXT, UI_PAGE_PREV } ui_action;

typedef struct {
    const char        *title;
    const char *const *items;
    int                count;
    int                page;
    int                x, y, w, h;    /* panel region */
    int                row_h;         /* derived; row 0 is the title */
    int                rows;          /* items per page */

    /* Edge state. A tap is accepted on touch-down and not accepted again
       until release: level-triggered would fire ~60 times a second. This is
       the d-pad's hysteresis lesson in a different costume. */
    bool               prev_touch;
    uint16_t           prev_buttons;
} koboy_ui_list;

void      ui_list_init(koboy_ui_list *u, const char *title,
                       const char *const *items, int count,
                       int x, int y, int w, int h);
int       ui_list_rows(const koboy_ui_list *u);
int       ui_list_pages(const koboy_ui_list *u);
void      ui_list_render(const koboy_ui_list *u, uint8_t *fb, int stride,
                         int W, int H);
ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st,
                       int *out_index);
#endif
```

- [ ] **Step 4: Write the implementation**

Create `src/ui.c`:

```c
#include "ui.h"
#include "text.h"
#include <string.h>

#define UI_BG        0xFF
#define UI_INK       0x00
#define UI_RULE      0xAA
#define UI_MAX_ROWS  10
#define UI_TEXT_PX   3

void ui_list_init(koboy_ui_list *u, const char *title,
                  const char *const *items, int count,
                  int x, int y, int w, int h)
{
    memset(u, 0, sizeof *u);
    u->title = title;
    u->items = items;
    u->count = count < 0 ? 0 : count;
    u->x = x; u->y = y; u->w = w; u->h = h;

    /* One title row plus one footer row, so the divisor is rows + 2. Clamped
       to at least one row so a tiny region still produces usable geometry
       instead of a division that yields zero and a widget nothing can hit. */
    int slots = UI_MAX_ROWS + 2;
    u->row_h = h / slots;
    if (u->row_h < TEXT_GLYPH_H * UI_TEXT_PX) u->row_h = TEXT_GLYPH_H * UI_TEXT_PX;
    u->rows = (h / u->row_h) - 2;
    if (u->rows < 1) u->rows = 1;
    if (u->rows > UI_MAX_ROWS) u->rows = UI_MAX_ROWS;
}

int ui_list_rows(const koboy_ui_list *u) { return u->rows; }

int ui_list_pages(const koboy_ui_list *u)
{
    if (u->count <= 0) return 1;
    return (u->count + u->rows - 1) / u->rows;
}

/* Index of the item drawn on row `r` of the current page, or -1 if that row is
   past the end of the list. The last page is short, and a tap on one of its
   dead rows must select nothing rather than an item that does not exist. */
static int item_at_row(const koboy_ui_list *u, int r)
{
    if (r < 0 || r >= u->rows) return -1;
    int i = u->page * u->rows + r;
    return (i >= 0 && i < u->count) ? i : -1;
}

void ui_list_render(const koboy_ui_list *u, uint8_t *fb, int stride,
                    int W, int H)
{
    /* Clear the region, clamped: the caller passes panel geometry, and a
       region running past an edge would otherwise memset past the buffer. */
    for (int y = u->y; y < u->y + u->h; y++) {
        if (y < 0 || y >= H) continue;
        int x0 = u->x < 0 ? 0 : u->x;
        int x1 = u->x + u->w;
        if (x1 > W) x1 = W;
        if (x0 < x1) memset(fb + (size_t)y * stride + x0, UI_BG, (size_t)(x1 - x0));
    }

    text_draw(fb, stride, W, H, u->x + u->row_h / 2, u->y + u->row_h / 4,
              u->title, UI_TEXT_PX + 1, UI_INK);

    for (int r = 0; r < u->rows; r++) {
        int i = item_at_row(u, r);
        if (i < 0) break;
        int ry = u->y + u->row_h + r * u->row_h;
        text_draw(fb, stride, W, H, u->x + u->row_h, ry + u->row_h / 4,
                  u->items[i], UI_TEXT_PX, UI_INK);
        /* Row rule, so a finger can tell where one entry ends. */
        int ly = ry + u->row_h - 1;
        if (ly >= 0 && ly < H) {
            int x0 = u->x < 0 ? 0 : u->x;
            int x1 = u->x + u->w; if (x1 > W) x1 = W;
            if (x0 < x1) memset(fb + (size_t)ly * stride + x0, UI_RULE, (size_t)(x1 - x0));
        }
    }

    /* Footer: page indicator plus the arrows a touch-only Kobo taps. */
    char foot[64];
    int page = u->page + 1, pages = ui_list_pages(u);
    foot[0] = 0;
    {
        const char *d = "0123456789";
        char tmp[64]; int n = 0;
        tmp[n++] = '<'; tmp[n++] = ' ';
        if (page >= 10) tmp[n++] = d[(page / 10) % 10];
        tmp[n++] = d[page % 10];
        tmp[n++] = '/';
        if (pages >= 10) tmp[n++] = d[(pages / 10) % 10];
        tmp[n++] = d[pages % 10];
        tmp[n++] = ' '; tmp[n++] = '>';
        tmp[n] = 0;
        memcpy(foot, tmp, (size_t)n + 1);
    }
    text_draw_centred(fb, stride, W, H, u->y + u->h - u->row_h + u->row_h / 4,
                      foot, UI_TEXT_PX, UI_INK);
}

/* First touch slot that is down, or -1. */
static int first_down(const koboy_input_state *st)
{
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++)
        if (st->touch[s].down) return s;
    return -1;
}

static void page_by(koboy_ui_list *u, int delta)
{
    int p = u->page + delta;
    int max = ui_list_pages(u) - 1;
    if (p < 0) p = 0;
    if (p > max) p = max;
    u->page = p;
}

ui_action ui_list_feed(koboy_ui_list *u, const koboy_input_state *st,
                       int *out_index)
{
    ui_action act = UI_NONE;

    /* Buttons first, on the rising edge only. A and B are the calibrated
       page-turn keys; in a UI mode they mean previous and next page, which is
       what the hardware is named after. */
    uint16_t rising = (uint16_t)(st->buttons & ~u->prev_buttons);
    u->prev_buttons = st->buttons;

    int slot = first_down(st);
    bool touching = (slot >= 0);
    bool tap = touching && !u->prev_touch;
    u->prev_touch = touching;

    if (rising & KOBOY_BTN_A) {
        int before = u->page;
        page_by(u, -1);
        if (u->page != before) return UI_PAGE_PREV;
        return UI_NONE;
    }
    if (rising & KOBOY_BTN_B) {
        int before = u->page;
        page_by(u, +1);
        if (u->page != before) return UI_PAGE_NEXT;
        return UI_NONE;
    }

    if (!tap) return UI_NONE;

    int tx = st->touch[slot].x, ty = st->touch[slot].y;
    if (tx < u->x || tx >= u->x + u->w || ty < u->y || ty >= u->y + u->h)
        return UI_NONE;

    /* Footer arrows: left third is previous, right third is next. */
    int foot_top = u->y + u->h - u->row_h;
    if (ty >= foot_top) {
        int before = u->page;
        if (tx < u->x + u->w / 3)            page_by(u, -1);
        else if (tx > u->x + 2 * u->w / 3)   page_by(u, +1);
        else                                 return UI_NONE;
        if (u->page == before) return UI_NONE;
        return (u->page > before) ? UI_PAGE_NEXT : UI_PAGE_PREV;
    }

    int r = (ty - u->y - u->row_h) / u->row_h;
    if (ty < u->y + u->row_h) return UI_NONE;      /* the title is not a row */
    int i = item_at_row(u, r);
    if (i < 0) return UI_NONE;
    if (out_index) *out_index = i;
    act = UI_SELECT;
    return act;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/test_ui && ./build/test_ui`
Expected: PASS — `tests/test_ui.c: 27 checks, 0 failures`

- [ ] **Step 6: Verify the edge detection is real (mutant)**

In `ui_list_feed`, change `bool tap = touching && !u->prev_touch;` to
`bool tap = touching;`. Rebuild and run.
Expected: the two `CHECK_EQ_INT(ui_list_feed(&u, &down, &idx), UI_NONE)` checks
after the first select FAIL, because a held touch now selects on every poll.
Revert. Record the output.

- [ ] **Step 7: Verify the page-offset indexing is real (mutant)**

In `item_at_row`, change `int i = u->page * u->rows + r;` to `int i = r;`.
Rebuild and run.
Expected: `CHECK_EQ_INT(idx, rows)` FAILS — a selection on page 1 returns an
index from page 0, i.e. the wrong ROM loads. Revert. Record the output.

- [ ] **Step 8: Commit**

```bash
git add src/ui.h src/ui.c tests/test_ui.c
git commit -m "feat: one list widget for both the ROM browser and the menu

The browser and the menu are the same thing: a titled list of strings you
tap, with paging. It consumes koboy_input_state rather than evdev events,
so the tests are real host unit tests instead of device theatre.

Two mutants recorded: dropping the touch rising-edge makes a held finger
select ~60 times a second, and dropping the page offset in item_at_row
loads the wrong ROM from any page but the first."
```

---
---

### Task 4: The ROM directory scan

**Files:**
- Create: `src/romlist.h`
- Create: `src/romlist.c`
- Create: `tests/test_romlist.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `#define ROMLIST_MAX 256`, `ROMLIST_NAME 128`
  - `bool romlist_is_rom(const char *name)`
  - `int romlist_scan(koboy_romlist *rl, const char *dir)` — returns count, or -1 if the directory cannot be opened
  - `const char *romlist_name(const koboy_romlist *rl, int i)`
  - `void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n)`
  - `const char *const *romlist_items(const koboy_romlist *rl)` — the array `ui_list_init` wants

- [ ] **Step 1: Write the failing test**

Create `tests/test_romlist.c`:

```c
#include "test.h"
#include "romlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void touch_file(const char *dir, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) { fputc('x', f); fclose(f); }
}

TEST_MAIN({
    /* Pure predicate first: extension matching is the whole filter. */
    CHECK_EQ_INT(romlist_is_rom("ZELDA.gb"), 1);
    CHECK_EQ_INT(romlist_is_rom("ZELDA.GB"), 1);
    CHECK_EQ_INT(romlist_is_rom("KIRBY.gbc"), 1);
    CHECK_EQ_INT(romlist_is_rom("KIRBY.GBC"), 1);
    CHECK_EQ_INT(romlist_is_rom("SAVE.srm"), 0);
    CHECK_EQ_INT(romlist_is_rom("NOTES.txt"), 0);
    CHECK_EQ_INT(romlist_is_rom("koboy.ini"), 0);
    /* A bare ".gb" has no stem; still a rom by extension, and the browser
       must not crash on it. */
    CHECK_EQ_INT(romlist_is_rom(".gb"), 1);
    /* Names shorter than the extension must not read before the string. */
    CHECK_EQ_INT(romlist_is_rom("g"), 0);
    CHECK_EQ_INT(romlist_is_rom(""), 0);
    /* A dotfile that merely CONTAINS gb is not a rom. */
    CHECK_EQ_INT(romlist_is_rom("gbfile"), 0);

    /* A missing directory is reported, not treated as empty: "you have no
       ROMs" and "your rom_dir is wrong" are different diagnoses, and on a
       device with no terminal that distinction is the whole diagnostic. */
    koboy_romlist rl;
    CHECK_EQ_INT(romlist_scan(&rl, "/nonexistent/koboy/test/dir"), -1);

    /* Scan a real directory. */
    char dir[] = "/tmp/koboy_romlist_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    touch_file(dir, "TETRIS.gb");
    touch_file(dir, "zelda.GB");
    touch_file(dir, "KIRBY 2.gbc");
    touch_file(dir, "notes.txt");
    touch_file(dir, "TETRIS.srm");

    int n = romlist_scan(&rl, dir);
    CHECK_EQ_INT(n, 3);

    /* Sorted case-insensitively, so the list reads the way a person expects
       rather than the way readdir happened to return it. */
    CHECK(strcmp(romlist_name(&rl, 0), "KIRBY 2.gbc") == 0);
    CHECK(strcmp(romlist_name(&rl, 1), "TETRIS.gb") == 0);
    CHECK(strcmp(romlist_name(&rl, 2), "zelda.GB") == 0);

    /* Full paths join the directory back on. */
    char path[512];
    romlist_path(&rl, 1, path, sizeof path);
    char want[512];
    snprintf(want, sizeof want, "%s/TETRIS.gb", dir);
    CHECK(strcmp(path, want) == 0);

    /* Out-of-range indices are safe, not undefined. The UI derives an index
       from a touch, so a stale page after a rescan is reachable. */
    CHECK(romlist_name(&rl, -1) != NULL);
    CHECK(romlist_name(&rl, 999) != NULL);
    path[0] = 'Z';
    romlist_path(&rl, 999, path, sizeof path);
    CHECK_EQ_INT(path[0], 0);

    /* items() hands ui_list_init an array it can index directly. */
    const char *const *items = romlist_items(&rl);
    CHECK(strcmp(items[0], "KIRBY 2.gbc") == 0);

    /* An empty directory scans to zero without failing. */
    char empty[] = "/tmp/koboy_romlist_e_XXXXXX";
    CHECK(mkdtemp(empty) != NULL);
    CHECK_EQ_INT(romlist_scan(&rl, empty), 0);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", dir, empty);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_romlist`
Expected: FAIL — `fatal error: romlist.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `src/romlist.h`:

```c
#ifndef KOBOY_ROMLIST_H
#define KOBOY_ROMLIST_H
#include <stdbool.h>
#include <stddef.h>

#define ROMLIST_MAX  256
#define ROMLIST_NAME 128

/* names[] is an array of fixed-width buffers, and item_ptr[] is an array of
   pointers INTO it, because ui_list_init wants `const char *const *` and a
   2-D char array is not that. Kept together so the two cannot drift. */
typedef struct {
    char        dir[512];
    char        names[ROMLIST_MAX][ROMLIST_NAME];
    const char *item_ptr[ROMLIST_MAX];
    int         count;
} koboy_romlist;

/* True for a name ending .gb or .gbc, either case. Pure, so the filter is
   tested without a filesystem. */
bool romlist_is_rom(const char *name);

/* Scans `dir` for ROMs, sorted case-insensitively. Returns the count, or -1 if
   the directory cannot be opened -- which is a different answer from 0 and must
   stay that way: "you have no ROMs" and "your rom_dir is wrong" are different
   diagnoses to a user with no terminal. */
int  romlist_scan(koboy_romlist *rl, const char *dir);

/* Always returns a valid C string; an out-of-range index yields "". */
const char *romlist_name(const koboy_romlist *rl, int i);

/* Writes dir/name into out. An out-of-range index writes an empty string. */
void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n);

const char *const *romlist_items(const koboy_romlist *rl);
#endif
```

- [ ] **Step 4: Write the implementation**

Create `src/romlist.c`:

```c
#define _POSIX_C_SOURCE 200809L
#include "romlist.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Own case-insensitive compare rather than strcasecmp: this project keeps its
   portable code free of anything the host might not have, and the comparison
   is four lines. */
static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    /* Guard is live: readdir returns names shorter than the suffix (".", ".."),
       and an unguarded s + ls - lx would read before the string. */
    if (lx > ls) return false;
    return ci_cmp(s + ls - lx, suffix) == 0;
}

bool romlist_is_rom(const char *name)
{
    if (!name || !*name) return false;
    return ends_with_ci(name, ".gb") || ends_with_ci(name, ".gbc");
}

static int name_cmp(const void *a, const void *b)
{
    return ci_cmp((const char *)a, (const char *)b);
}

static void rebuild_ptrs(koboy_romlist *rl)
{
    for (int i = 0; i < rl->count; i++) rl->item_ptr[i] = rl->names[i];
}

int romlist_scan(koboy_romlist *rl, const char *dir)
{
    memset(rl, 0, sizeof *rl);
    snprintf(rl->dir, sizeof rl->dir, "%s", dir);

    DIR *d = opendir(dir);
    if (!d) return -1;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (rl->count >= ROMLIST_MAX) break;   /* cap; see the note below */
        if (!romlist_is_rom(e->d_name)) continue;
        snprintf(rl->names[rl->count], ROMLIST_NAME, "%s", e->d_name);
        rl->count++;
    }
    closedir(d);

    qsort(rl->names, (size_t)rl->count, ROMLIST_NAME, name_cmp);
    rebuild_ptrs(rl);
    return rl->count;
}

const char *romlist_name(const koboy_romlist *rl, int i)
{
    if (i < 0 || i >= rl->count) return "";
    return rl->names[i];
}

void romlist_path(const koboy_romlist *rl, int i, char *out, size_t n)
{
    if (n == 0) return;
    if (i < 0 || i >= rl->count) { out[0] = 0; return; }
    snprintf(out, n, "%s/%s", rl->dir, rl->names[i]);
}

const char *const *romlist_items(const koboy_romlist *rl)
{
    return rl->item_ptr;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/test_romlist && ./build/test_romlist`
Expected: PASS — `tests/test_romlist.c: 22 checks, 0 failures`

- [ ] **Step 6: Verify the short-name guard is real (mutant)**

Delete `if (lx > ls) return false;` from `ends_with_ci`, rebuild, run under
ASan: `cc -std=c11 -fsanitize=address -Isrc -Itests -o /tmp/t_rl tests/test_romlist.c src/*.c -lm -ldl 2>/dev/null; /tmp/t_rl`

Note: that command pulls in `main.c`/`probe.c`, which will not link. Instead
build the sanitised binary the same way the Makefile does:

```bash
cc -std=c11 -fsanitize=address -g -Isrc -Itests -o /tmp/t_rl \
   tests/test_romlist.c $(ls src/*.c | grep -vE 'src/(main|probe|platform_)') -lm -ldl
/tmp/t_rl
```

Expected: ASan reports a heap-buffer-underflow on `romlist_is_rom("g")` or
`romlist_is_rom("")`. Revert. Record the output.

- [ ] **Step 7: Note the cap honestly**

The `ROMLIST_MAX` cap silently drops ROMs beyond 256. Add the log line so a
truncated list is never mistaken for a complete one — silent truncation reads
as "we listed everything" when it did not. In `romlist_scan`, replace the
`break;` with:

```c
        if (rl->count >= ROMLIST_MAX) {
            /* Not silent. A browser that quietly shows 256 of 300 ROMs is a
               browser the user thinks is broken for one specific game. */
            fprintf(stderr, "koboy: more than %d roms in %s, listing the "
                            "first %d\n", ROMLIST_MAX, dir, ROMLIST_MAX);
            break;
        }
```

- [ ] **Step 8: Re-run and commit**

Run: `make test`
Expected: all binaries report `0 failures`.

```bash
git add src/romlist.h src/romlist.c tests/test_romlist.c
git commit -m "feat: romlist -- scan a directory for .gb/.gbc, sorted

A missing directory returns -1 rather than 0, because 'you have no ROMs'
and 'your rom_dir is wrong' are different diagnoses to a user with no
terminal. The 256-entry cap logs when it truncates: a browser that
quietly shows 256 of 300 ROMs is one the user thinks is broken for one
specific game.

Mutant: removing the length guard in ends_with_ci makes ASan report a
heap-buffer-underflow on romlist_is_rom(\"g\")."
```

---

### Task 5: The UI script, so scripted runs can reach the new screens

This task exists because of v1's recorded endgame: *the first-run deadlock was
invisible to twenty per-task reviews because the scripted-run branch skips
calibration — every automated test took the one path that could not reach the
bug.* Every existing smoke test passes `--rom`, so every existing smoke test
would skip `MODE_BROWSE`. This is the answer, and it lands **with** the browser
in Task 6, not after it.

**Files:**
- Create: `src/uiscript.h`
- Create: `src/uiscript.c`
- Create: `tests/test_uiscript.c`

**Interfaces:**
- Consumes: `koboy_input_state` (`src/koboy.h`).
- Produces:
  - `#define UISCRIPT_MAX 256`
  - `int uiscript_load(const char *path, koboy_input_state *out, int max)` — returns the number of states, or -1 on error

- [ ] **Step 1: Write the failing test**

Create `tests/test_uiscript.c`:

```c
#include "test.h"
#include "uiscript.h"
#include <stdio.h>
#include <stdlib.h>

static void write_script(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

TEST_MAIN({
    char dir[] = "/tmp/koboy_uiscript_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/s.txt", dir);

    /* Each `tap` becomes TWO states -- press then release -- because
       ui_list_feed is edge triggered and a press with no release would latch
       the widget for the rest of the run. */
    write_script(path,
        "# a comment, and a blank line follow\n"
        "\n"
        "tap 400 900\n"
        "key b\n"
        "idle 3\n"
        "key a\n");

    static koboy_input_state st[UISCRIPT_MAX];
    int n = uiscript_load(path, st, UISCRIPT_MAX);

    /* tap = 2, key b = 2, idle 3 = 3, key a = 2  ->  9 */
    CHECK_EQ_INT(n, 9);

    CHECK_EQ_INT(st[0].touch[0].down, 1);
    CHECK_EQ_INT(st[0].touch[0].x, 400);
    CHECK_EQ_INT(st[0].touch[0].y, 900);
    CHECK_EQ_INT(st[1].touch[0].down, 0);

    CHECK_EQ_INT(st[2].buttons, KOBOY_BTN_B);
    CHECK_EQ_INT(st[3].buttons, 0);

    CHECK_EQ_INT(st[4].buttons, 0);
    CHECK_EQ_INT(st[4].touch[0].down, 0);
    CHECK_EQ_INT(st[6].buttons, 0);

    CHECK_EQ_INT(st[7].buttons, KOBOY_BTN_A);
    CHECK_EQ_INT(st[8].buttons, 0);

    /* A missing file is an error, not an empty script: a typo in --ui-script
       must fail the run rather than silently pass a test that exercised
       nothing. */
    CHECK_EQ_INT(uiscript_load("/nonexistent/koboy/script", st, UISCRIPT_MAX), -1);

    /* An unknown verb is an error for the same reason. */
    write_script(path, "wiggle 1 2\n");
    CHECK_EQ_INT(uiscript_load(path, st, UISCRIPT_MAX), -1);

    /* A malformed tap is an error. */
    write_script(path, "tap 400\n");
    CHECK_EQ_INT(uiscript_load(path, st, UISCRIPT_MAX), -1);

    /* Overflow truncates rather than overruns. */
    {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        for (int i = 0; i < UISCRIPT_MAX; i++) fputs("key a\n", f);
        fclose(f);
        int got = uiscript_load(path, st, UISCRIPT_MAX);
        CHECK(got > 0);
        CHECK(got <= UISCRIPT_MAX);
    }

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_uiscript`
Expected: FAIL — `fatal error: uiscript.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `src/uiscript.h`:

```c
#ifndef KOBOY_UISCRIPT_H
#define KOBOY_UISCRIPT_H
#include "koboy.h"

/* Replays synthetic input states into the UI modes, so a bounded, unattended
   run can reach MODE_BROWSE and MODE_MENU.

   This exists because of a recorded v1 failure: the first-run deadlock was
   invisible to twenty reviews because the scripted-run branch skipped
   calibration, so every automated test took the one path that could not reach
   the bug. Every existing smoke test passes --rom and would therefore skip the
   browser entirely. When a code path exists only for scripted runs, ask what
   it is hiding.

   Grammar, one verb per line; `#` starts a comment:
     tap X Y   press at panel (X, Y), then release   -> 2 states
     key a     press A (page previous), then release -> 2 states
     key b     press B (page next), then release     -> 2 states
     idle N    N states with nothing pressed         -> N states */

#define UISCRIPT_MAX 256

/* Returns the number of states written, or -1 on any error: a missing file, an
   unknown verb, or a malformed argument. An error must fail the run rather
   than silently pass a test that exercised nothing. */
int uiscript_load(const char *path, koboy_input_state *out, int max);
#endif
```

- [ ] **Step 4: Write the implementation**

Create `src/uiscript.c`:

```c
#define _POSIX_C_SOURCE 200809L
#include "uiscript.h"
#include <stdio.h>
#include <string.h>

static int push(koboy_input_state *out, int *n, int max,
                const koboy_input_state *st)
{
    if (*n >= max) return 0;                 /* truncate, never overrun */
    out[(*n)++] = *st;
    return 1;
}

static void clear(koboy_input_state *st) { memset(st, 0, sizeof *st); }

int uiscript_load(const char *path, koboy_input_state *out, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    int n = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;

        char verb[32];
        int a = 0, b = 0;
        int fields = sscanf(line, "%31s %d %d", verb, &a, &b);
        if (fields < 1) continue;            /* blank or comment-only */

        koboy_input_state st;
        if (!strcmp(verb, "tap")) {
            if (fields != 3) { fclose(f); return -1; }
            clear(&st);
            st.touch[0].x = a; st.touch[0].y = b; st.touch[0].down = true;
            if (!push(out, &n, max, &st)) break;
            clear(&st);
            if (!push(out, &n, max, &st)) break;
        } else if (!strcmp(verb, "key")) {
            /* sscanf put the key letter nowhere, so re-read it: "key a" has
               fields == 1 and the letter is still in the line. */
            char which = 0;
            if (sscanf(line, "%31s %c", verb, &which) != 2) { fclose(f); return -1; }
            uint16_t bit;
            if      (which == 'a' || which == 'A') bit = KOBOY_BTN_A;
            else if (which == 'b' || which == 'B') bit = KOBOY_BTN_B;
            else { fclose(f); return -1; }
            clear(&st);
            st.buttons = bit;
            if (!push(out, &n, max, &st)) break;
            clear(&st);
            if (!push(out, &n, max, &st)) break;
        } else if (!strcmp(verb, "idle")) {
            if (fields < 2 || a < 0) { fclose(f); return -1; }
            clear(&st);
            int pushed = 1;
            for (int i = 0; i < a && pushed; i++)
                pushed = push(out, &n, max, &st);
            if (!pushed) break;
        } else {
            fclose(f);
            return -1;                        /* unknown verb */
        }
    }
    fclose(f);
    return n;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/test_uiscript && ./build/test_uiscript`
Expected: PASS — `tests/test_uiscript.c: 20 checks, 0 failures`

- [ ] **Step 6: Verify the release state is real (mutant)**

In the `tap` branch, delete the second `clear(&st); push(...)` pair so a tap
emits only a press. Rebuild and run.
Expected: `CHECK_EQ_INT(n, 9)` FAILS (8 states), and `st[1].touch[0].down`
is wrong. Revert. This mutant matters because without the release the widget
latches and every later scripted tap does nothing — the script would appear to
run while testing exactly one interaction. Record the output.

- [ ] **Step 7: Commit**

```bash
git add src/uiscript.h src/uiscript.c tests/test_uiscript.c
git commit -m "feat: uiscript, so a scripted run can reach the UI modes

v1's first-run deadlock was invisible to twenty reviews because the
scripted branch skipped calibration -- every automated test took the one
path that could not reach the bug. Every existing smoke test passes
--rom, so every one of them would skip MODE_BROWSE the same way. This is
the answer, and it lands with the browser rather than after it.

Mutant: emitting a press with no release makes the tap count wrong and
latches the widget, so every later scripted tap silently does nothing."
```

---

### Task 6: The mode machine and `MODE_BROWSE`

Closes follow-up **#5** (ROM failure paths stop being theoretical the moment a
user can tap a truncated download) and **#14** (`platform_kobo.h`).

**Files:**
- Create: `src/platform_kobo.h`
- Modify: `src/video.h`, `src/video.c` (add `video_invalidate`)
- Modify: `src/config.h`, `src/config.c` (add `rom_dir`)
- Modify: `src/main.c` (mode machine, `redraw_chrome`, `--ui-script`, `--rom-dir`)
- Modify: `src/platform_kobo.c` (include the new header instead of re-declaring)
- Modify: `tests/test_video_dirty.c` (cover `video_invalidate`)
- Modify: `tests/smoke_host.sh` (a scripted run that boots into the browser)

**Interfaces:**
- Consumes: `ui_list_*` (Task 3), `romlist_*` (Task 4), `uiscript_load` (Task 5),
  `text_draw_centred` (Task 2).
- Produces:
  - `void video_invalidate(koboy_video *v)`
  - `koboy_config.rom_dir[512]`, ini key `rom_dir`, CLI `--rom-dir PATH`
  - CLI `--ui-script PATH`
  - `src/platform_kobo.h` declaring `platform_kobo_create`,
    `platform_kobo_setup_touch`, `platform_kobo_selftest`,
    `platform_kobo_refresh_stats`, `platform_kobo_fatal`

- [ ] **Step 1: Write the failing test for `video_invalidate`**

Append to `tests/test_video_dirty.c`, inside its `TEST_MAIN({ ... })` body:

```c
    /* video_invalidate forces the NEXT submit to report the whole game rect
       dirty. Required because a UI mode paints over the game rect, so the
       prev buffer no longer describes what is on the panel -- without this the
       first frame back diffs against a screen that is gone and silently leaves
       chrome-covered pixels stale. */
    {
        koboy_profile p; koboy_config c;
        config_defaults(&c);
        config_resolve_profile(&p, &c, 1264, 1680);
        koboy_video *v = video_create(&p, false);
        CHECK(v != NULL);

        static uint16_t frame[KOBOY_GB_W * KOBOY_GB_H];
        for (int i = 0; i < KOBOY_GB_W * KOBOY_GB_H; i++) frame[i] = 0x0000;

        koboy_rect r1 = video_submit(v, frame, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * 2, KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(r1.w, p.game_w);          /* first frame is fully dirty */

        /* The same frame again changes nothing. */
        koboy_rect r2 = video_submit(v, frame, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * 2, KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(r2.w, 0);

        /* After invalidation the identical frame is fully dirty again. */
        video_invalidate(v);
        koboy_rect r3 = video_submit(v, frame, KOBOY_GB_W, KOBOY_GB_H,
                                     KOBOY_GB_W * 2, KOBOY_PIXFMT_RGB565);
        CHECK_EQ_INT(r3.w, p.game_w);
        CHECK_EQ_INT(r3.h, p.game_h);
        CHECK_EQ_INT(r3.x, 0);
        CHECK_EQ_INT(r3.y, 0);

        video_destroy(v);
    }
```

Ensure `tests/test_video_dirty.c` includes `"config.h"` and `"video.h"`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_video_dirty`
Expected: FAIL — `implicit declaration of function 'video_invalidate'`

- [ ] **Step 3: Implement `video_invalidate`**

In `src/video.h`, after the `video_submit` declaration:

```c
/* Forces the next video_submit to report the entire game rect dirty.
   Called on the way back from any UI mode: those modes paint over the game
   rect, so `prev` stops describing what is on the panel. Without this the
   first frame back diffs against a screen that is no longer there and leaves
   the overpainted region stale -- which looks exactly like ghosting and is
   therefore the kind of bug nobody reports as a bug. */
void video_invalidate(koboy_video *v);
```

In `src/video.c`, after `video_destroy`:

```c
void video_invalidate(koboy_video *v)
{
    if (!v) return;
    /* 0x01 is not a value the four-level quantiser can ever emit, which is the
       same trick video_create uses to make the first frame fully dirty. */
    memset(v->prev, 0x01, (size_t)v->stride * (size_t)v->p.game_h);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make build/test_video_dirty && ./build/test_video_dirty`
Expected: PASS.

- [ ] **Step 5: Verify the invalidation is real (mutant)**

Change `video_invalidate`'s body to `(void)v;`. Rebuild and run.
Expected: `CHECK_EQ_INT(r3.w, p.game_w)` FAILS with `r3.w == 0`. Revert.
Record the output.

- [ ] **Step 6: Add `rom_dir` to the config**

In `src/config.h`, add to `koboy_config` immediately after `char rom_path[512];`:

```c
    char     rom_dir[512];       /* where the browser looks; install-relative */
```

In `src/config.c`, inside `config_defaults`, beside the other path defaults:

```c
    snprintf(c->rom_dir, sizeof c->rom_dir, "roms");
```

In `config_load`'s dispatch, beside the `rom` line:

```c
        else if (!strcmp(k, "rom_dir"))          snprintf(c->rom_dir,   sizeof c->rom_dir,   "%s", v);
```

In `config_resolve_paths`, beside the other three:

```c
    if (config_join_sibling(tmp, sizeof tmp, c->rom_dir, dir))
        snprintf(c->rom_dir, sizeof c->rom_dir, "%s", tmp);
```

- [ ] **Step 7: Create `src/platform_kobo.h` (follow-up #14)**

Create `src/platform_kobo.h`:

```c
#ifndef KOBOY_PLATFORM_KOBO_H
#define KOBOY_PLATFORM_KOBO_H
#include "platform_if.h"

/* These prototypes were duplicated in src/main.c and src/platform_kobo.c with
   no shared header. They agreed, and nothing would have caught it if they
   stopped agreeing -- a silently mismatched declaration across a translation
   unit boundary is undefined behaviour that links cleanly. One definition,
   included by both. */

koboy_platform *platform_kobo_create(void);
void            platform_kobo_setup_touch(koboy_platform *pf, struct koboy_input *in);
void            platform_kobo_selftest(koboy_platform *pf);
void            platform_kobo_refresh_stats(koboy_platform *pf);
void            platform_kobo_fatal(void *ctx, const char *msg);
#endif
```

In `src/main.c`, replace the `#ifdef KOBOY_PLATFORM_KOBO` block of five
`extern` declarations with:

```c
#ifdef KOBOY_PLATFORM_KOBO
#include "platform_kobo.h"
#else
extern koboy_platform *platform_sdl_create(void);
extern void            platform_sdl_set_panel(koboy_platform *pf, int w, int h);
#endif
```

Leave `extern bool platform_poll_raw_key(koboy_platform *pf, uint16_t *code);`
where it is — both backends define it, so it is not Kobo-specific.

In `src/platform_kobo.c`, delete its own copy of the five prototypes (around
line 656) and add `#include "platform_kobo.h"` beside its other includes.

- [ ] **Step 8: Verify the header change compiles both backends**

Run: `make test && make host`
Expected: both succeed.

Run (only if the Linaro toolchain is on PATH — see `docs/cross-compiling.md`):
`make kobo`
Expected: `build/koboy-arm` and `build/koboy-probe-arm` build.

If the toolchain is absent, note that the ARM build was **not** verified in this
task and must be before the packaging task.

- [ ] **Step 9: Add the mode machine to `main.c`**

Add near the other includes:

```c
#include "romlist.h"
#include "ui.h"
#include "uiscript.h"
```

Add above `main()`:

```c
typedef enum { MODE_BROWSE, MODE_PLAY, MODE_MENU, MODE_QUIT } koboy_mode;

/* One definition of "put the faceplate back", replacing three hand-copied
   blocks (post-calibration, post-fatal, post-SRAM-warning) and now used by
   every exit from a UI mode as well. */
static void redraw_chrome(koboy_platform *pf, uint8_t *panel, int stride,
                          int pw, int ph, const koboy_profile *prof,
                          const koboy_layout *layout)
{
    memset(panel, 0xFF, (size_t)stride * (size_t)ph);
    chrome_render(panel, stride, prof, layout);
    pf->blit_gray8(pf->ctx, panel, pw, ph, stride, 0, 0);
    pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
}

/* Drives one list widget to a selection. Returns the chosen index, or -1 if
   the user quit, the run was stopped, or a script ran out.

   `script`/`script_n` make MODE_BROWSE and MODE_MENU reachable in a bounded
   unattended run. Without them every automated test would pass --rom and skip
   these screens entirely -- the same blind spot that hid v1's first-run
   deadlock through twenty reviews. */
static int run_list(koboy_platform *pf, koboy_input *in, koboy_ui_list *u,
                    uint8_t *panel, int stride, int pw, int ph,
                    const koboy_input_state *script, int script_n)
{
    int  chosen = -1;
    int  si = 0;
    bool need_draw = true;

    while (!g_stop && !pf->should_quit(pf->ctx)) {
        if (need_draw) {
            need_draw = false;
            memset(panel, 0xFF, (size_t)stride * (size_t)ph);
            ui_list_render(u, panel, stride, pw, ph);
            pf->blit_gray8(pf->ctx, panel, pw, ph, stride, 0, 0);
            /* FULL, i.e. GC16: a list is about to sit still, and the game
               rect's four-level ceiling does not apply to it. */
            pf->refresh(pf->ctx, 0, 0, pw, ph, KOBOY_REFRESH_FULL);
        }

        const koboy_input_state *st;
        if (script) {
            if (si >= script_n) break;      /* script exhausted: give up */
            st = &script[si++];
        } else {
            pf->poll_input(pf->ctx, in);
            st = input_state(in);
        }

        int idx = -1;
        ui_action a = ui_list_feed(u, st, &idx);
        if (a == UI_SELECT) { chosen = idx; break; }
        if (a == UI_PAGE_NEXT || a == UI_PAGE_PREV) need_draw = true;

        if (!script) usleep(5000);
    }
    return chosen;
}
```

- [ ] **Step 10: Parse the two new flags**

In `main()`'s argument loop, beside the other value-taking options:

```c
        else if (!strcmp(a, "--rom-dir"))  snprintf(cfg.rom_dir, sizeof cfg.rom_dir, "%s", argv[++i]);
        else if (!strcmp(a, "--ui-script")) ui_script_path = argv[++i];
```

Declare above the loop, beside `const char *message = NULL;`:

```c
    const char *ui_script_path = NULL;
    bool        rom_from_argv  = false;
```

Set `rom_from_argv = true;` inside the existing `--rom` branch, immediately
after the `snprintf`. Add both flags to `usage()`:

```c
        "  --rom-dir PATH    directory the ROM browser lists\n"
        "  --ui-script PATH  replay synthetic UI input (scripted runs)\n"
```

- [ ] **Step 11: Load the script and choose the entry mode**

After `config_resolve_paths(&cfg);` add nothing; the script load needs the
platform, so put this immediately **after** the `if (!cfg.rom_path[0])` block —
and replace that block entirely with:

```c
    static koboy_input_state ui_script[UISCRIPT_MAX];
    int ui_script_n = 0;
    if (ui_script_path) {
        ui_script_n = uiscript_load(ui_script_path, ui_script, UISCRIPT_MAX);
        if (ui_script_n < 0) {
            fatal("cannot read ui script %s", ui_script_path);
            pf->shutdown(pf->ctx);
            return 2;
        }
    }

    /* An explicit --rom or rom= goes straight to play, which keeps every
       existing smoke test, --frames run and scripted path behaving exactly as
       it did in v1. The shipped ini leaves rom commented out, so a real user
       starts in the browser. */
    koboy_mode mode = cfg.rom_path[0] ? MODE_PLAY : MODE_BROWSE;
    (void)rom_from_argv;
```

- [ ] **Step 12: Run the browser before the core is opened**

Insert immediately after the chrome is first drawn and after the calibration
block, and **before** `koboy_video *vid = video_create(...)`:

```c
    koboy_romlist roms;
    if (mode == MODE_BROWSE) {
        int n = romlist_scan(&roms, cfg.rom_dir);
        if (n < 0) {
            /* Distinct from "no roms": a wrong rom_dir and an empty one are
               different mistakes, and this is the only diagnostic a user with
               no terminal gets. */
            fatal("cannot read rom directory\n%s", cfg.rom_dir);
            free(panel); pf->shutdown(pf->ctx); return 2;
        }
        if (n == 0) {
            fatal("no .gb or .gbc files in\n%s", cfg.rom_dir);
            free(panel); pf->shutdown(pf->ctx); return 2;
        }

        koboy_input *ui_in = input_create(&cfg, &prof);
        if (!ui_in) { fatal("out of memory"); free(panel); pf->shutdown(pf->ctx); return 1; }
#ifdef KOBOY_PLATFORM_KOBO
        platform_kobo_setup_touch(pf, ui_in);
#else
        input_set_touch_transform(ui_in, pw, ph, false, false, false);
#endif
        koboy_ui_list list;
        ui_list_init(&list, "CHOOSE A GAME", romlist_items(&roms), n,
                     KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                     pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

        int pick = run_list(pf, ui_in, &list, panel, panel_stride, pw, ph,
                            ui_script_n > 0 ? ui_script : NULL, ui_script_n);
        input_destroy(ui_in);

        if (pick < 0) {
            say("koboy: no rom chosen, exiting\n");
            free(panel); pf->shutdown(pf->ctx); return 0;
        }
        romlist_path(&roms, pick, cfg.rom_path, sizeof cfg.rom_path);
        say("koboy: chose %s\n", cfg.rom_path);
        mode = MODE_PLAY;

        /* The browser painted over the faceplate. */
        redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
    }
    (void)mode;
```

- [ ] **Step 13: Replace the three hand-copied chrome restores**

Find the three places in `main()` that do
`memset(panel, ...); chrome_render(...); blit_gray8(...); refresh(..., KOBOY_REFRESH_FULL);`
— after calibration, after the SRAM-unreadable `fatal`, and the initial draw is
**not** one of them (it precedes the profile-dependent helper) — and replace
each body with:

```c
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
```

- [ ] **Step 14: Build and run the browser end to end on the host**

```bash
make host
mkdir -p /tmp/koboy_roms && : > "/tmp/koboy_roms/AAA TEST.gb"
printf 'idle 2\ntap 40 200\n' > /tmp/koboy_ui.txt
./build/koboy --core build/stub_core.so --rom-dir /tmp/koboy_roms \
              --ui-script /tmp/koboy_ui.txt --panel 1264x1680 --frames 30 2>&1 | tail -20
```

Expected: a line `koboy: chose /tmp/koboy_roms/AAA TEST.gb`, then the run
proceeds and prints `presented=`.

If the tap misses, print the widget geometry and adjust the tap's Y to
`KOBOY_CHROME_MARGIN + row_h + row_h/2`. Do **not** loosen the widget's
hit-testing to make the script pass.

- [ ] **Step 15: Add the scripted browser run to the smoke test**

Append to `tests/smoke_host.sh`, following the file's existing style for
setup, invocation and assertion:

```sh
# The browser is invisible to every other test in this suite, because they all
# pass --rom and take the MODE_PLAY shortcut. That is precisely the shape of
# the blind spot that hid v1's first-run deadlock through twenty reviews, so
# this run exists to take the other path.
romdir="$(mktemp -d)"
: > "$romdir/AAA TEST.gb"
script="$(mktemp)"
printf 'idle 2\ntap 40 200\n' > "$script"

out="$("$KOBOY" --core "$STUB" --rom-dir "$romdir" --ui-script "$script" \
                --panel 1264x1680 --frames 30 2>&1)"
echo "$out" | grep -q "chose $romdir/AAA TEST.gb" \
    || { echo "FAIL: browser did not select the only rom"; echo "$out"; exit 1; }
echo "$out" | grep -q '^presented=' \
    || { echo "FAIL: run did not reach the emulator loop"; echo "$out"; exit 1; }
rm -rf "$romdir" "$script"
echo "ok: rom browser"
```

Match the variable names the script already uses for the binary and the stub
core; if they differ from `$KOBOY`/`$STUB`, use the existing ones.

- [ ] **Step 16: Verify the scripted path is real (mutant)**

Change `run_list`'s script branch to always use `pf->poll_input` instead.
Run `bash tests/smoke_host.sh`.
Expected: the browser run hangs or times out and the assertion FAILS, proving
the script actually drives the selection. Revert. Record the output.

- [ ] **Step 17: Full verification**

```bash
make test
bash tests/smoke_host.sh
```
Expected: both pass.

- [ ] **Step 18: Commit**

```bash
git add src/platform_kobo.h src/video.h src/video.c src/config.h src/config.c \
        src/main.c src/platform_kobo.c tests/test_video_dirty.c tests/smoke_host.sh
git commit -m "feat: mode machine and the ROM browser

main.c becomes BROWSE/PLAY/MENU/QUIT over the list widget. An explicit
--rom or rom= still goes straight to PLAY, so every existing test behaves
exactly as before -- which is also why the browser needs --ui-script to be
reachable at all under test. When a code path exists only for scripted
runs, ask what it is hiding.

Adds video_invalidate, because a UI mode paints over the game rect and
the prev buffer then describes a screen that is gone; without it the
first frame back leaves stale pixels that look exactly like ghosting.

Closes follow-up #14 (platform_kobo.h: the duplicated prototypes agreed,
and nothing would have caught them ceasing to) and puts #5's ROM failure
paths on the main path, since a user can now tap a truncated download.

Mutants: a no-op video_invalidate leaves the next frame reporting zero
dirty pixels; ignoring the script in run_list hangs the smoke test."
```

---

### Task 7: Config hardening

Closes follow-ups **#6**, **#8**, **#9** and **#13**. Pure `config.c` work,
independently reviewable, and it lands before the MENU zone adds four more keys
to the same file.

**Files:**
- Modify: `src/config.c` (`as_bool` empty values, trailing newline in `config_save_keys`)
- Modify: `tests/test_config.c` (rename the seed key so the filter is exercised)
- Modify: `config/koboy.ini` (install-relative comments for `rom`, `rom_dir`, `save_dir`)

**Interfaces:**
- Consumes: nothing.
- Produces: no new symbols; `as_bool` gains a second parameter,
  `static bool as_bool(const char *v, bool dflt)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_config.c` inside `TEST_MAIN({ ... })`:

```c
    /* #8: a blanked value must not silently mean true. as_bool treated
       everything except "false" and "0" as true, including "", so
       `grab_input = ` turned the grab ON -- the opposite of what someone
       clearing a line intends, and unrecoverable without reading the source. */
    {
        char dir[] = "/tmp/koboy_cfg_XXXXXX";
        CHECK(mkdtemp(dir) != NULL);
        char path[512];
        snprintf(path, sizeof path, "%s/koboy.ini", dir);

        FILE *f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = \nforce_dither =   \n", f);
        fclose(f);

        koboy_config c; config_defaults(&c);
        bool grab_default = c.grab_input, dither_default = c.force_dither;
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, grab_default);
        CHECK_EQ_INT(c.force_dither, dither_default);

        /* Explicit values still work in both directions. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("grab_input = false\nforce_dither = true\n", f);
        fclose(f);
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.grab_input, 0);
        CHECK_EQ_INT(c.force_dither, 1);

        /* #9: an ini with NO trailing newline must not have the calibration
           block concatenated onto its last line. It is harmless today only
           because config_load truncates at the resulting '#', but it silently
           rewrites an unrelated line, against the whole point of preserving
           everything else. */
        f = fopen(path, "w");
        CHECK(f != NULL);
        fputs("scale = 4", f);            /* deliberately no newline */
        fclose(f);
        CHECK(config_save_keys(path, 193, 194));

        f = fopen(path, "r");
        CHECK(f != NULL);
        char first[256] = {0};
        CHECK(fgets(first, sizeof first, f) != NULL);
        fclose(f);
        /* The first line must still be exactly the scale line. */
        CHECK(strncmp(first, "scale = 4", 9) == 0);
        CHECK_EQ_INT((int)strlen(first), 10);   /* "scale = 4" plus '\n' */

        /* And the keys survived the round trip. */
        config_defaults(&c);
        CHECK(config_load(&c, path));
        CHECK_EQ_INT(c.scale, 4);
        CHECK_EQ_INT(c.key_a, 193);
        CHECK_EQ_INT(c.key_b, 194);

        char cmd[1024];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
    }
```

Ensure `tests/test_config.c` includes `<stdio.h>` and `<stdlib.h>`.

- [ ] **Step 2: Fix the existing preservation test's seed key (#6)**

In `tests/test_config.c` around lines 121-134, the preservation test seeds a key
literally named `old key_a`. The exact-`strcmp` filter never matches that, so the
line is preserved trivially and the filter itself is never exercised.

Change the seed line from `old key_a` to a real `key_a` line plus an unrelated
key that must survive, and assert both outcomes:

```c
    /* Seeds a REAL key_a line, so config_save_keys' filter has something to
       filter. The previous seed was literally "old key_a", which the exact
       strcmp never matched -- the test passed whether or not the filter
       existed. */
    fputs("key_a = 99\nscale = 3\nkey_b = 98\n", f);
```

and after the save, assert that `scale = 3` is still present, that `key_a = 99`
is **gone**, and that exactly one `key_a` and one `key_b` line exist.

- [ ] **Step 3: Run tests to verify they fail**

Run: `make build/test_config && ./build/test_config`
Expected: FAIL on the blank-value checks (`c.grab_input` is 1 where the default
is expected), on the trailing-newline length check, and on the `key_a = 99`
absence check.

- [ ] **Step 4: Fix `as_bool`**

In `src/config.c`, replace:

```c
static bool as_bool(const char *v) { return !(strcmp(v,"false")==0 || strcmp(v,"0")==0); }
```

with:

```c
/* An EMPTY value leaves the default alone rather than meaning true. This
   treated everything except "false" and "0" as true, including "", so a
   blanked `grab_input = ` silently turned the grab on -- the opposite of what
   clearing a line means, and invisible without reading this function. `trim`
   has already run, so "" covers whitespace-only values too. */
static bool as_bool(const char *v, bool dflt) {
    if (!v || !v[0]) return dflt;
    return !(strcmp(v,"false")==0 || strcmp(v,"0")==0);
}
```

Update the two call sites in `config_load`:

```c
        else if (!strcmp(k, "force_dither"))     c->force_dither = as_bool(v, c->force_dither);
        else if (!strcmp(k, "grab_input"))       c->grab_input   = as_bool(v, c->grab_input);
```

- [ ] **Step 5: Fix the missing trailing newline**

In `src/config_save_keys`'s copy loop, track whether the last byte written was a
newline and emit one before the appended block. Immediately after the loop that
copies the filtered input lines and before the `key_a`/`key_b` lines are written:

```c
    /* A source ini with no trailing newline would otherwise have the
       calibration block concatenated onto its final line. Harmless today only
       because config_load truncates at the resulting '#', but it silently
       rewrites an unrelated line -- against the "preserve everything else"
       intent this function exists to honour. */
    if (last_char_written && last_char_written != '\n') fputc('\n', out);
```

Declare `int last_char_written = 0;` before the loop and set it from the final
byte of each line as it is copied.

- [ ] **Step 6: Run tests to verify they pass**

Run: `make build/test_config && ./build/test_config`
Expected: PASS.

- [ ] **Step 7: Verify both fixes are real (mutants)**

1. Revert `as_bool` to the one-argument version (call sites `as_bool(v)`).
   Expected: the blank-value checks FAIL. Revert the mutant.
2. Delete the `if (last_char_written && ...)` line.
   Expected: the `strlen(first) == 10` check FAILS because the first line has
   the calibration comment concatenated onto it. Revert the mutant.
3. In `config_save_keys`, make the filter never match (change its `strcmp` to
   compare against a nonsense key).
   Expected: the "exactly one `key_a` line" check FAILS. Revert.

Record all three outputs.

- [ ] **Step 8: Document install-relative resolution in the ini (#13)**

In `config/koboy.ini`, the `core` comment explains install-relative resolution
but `rom` and `save_dir` do not, even though `config_resolve_paths` applies the
same rule to all of them. Add to each of `rom`, `rom_dir` and `save_dir`:

```
# A bare name with no "/" is resolved against the directory containing the
# koboy executable, exactly like `core` above -- not against the current
# directory, which a NickelMenu or KFMon launch does not set.
```

Add the `rom_dir` key itself while here:

```
# Directory the ROM browser lists. Only used when `rom` is unset: an explicit
# rom= skips the browser entirely and loads that file.
rom_dir = roms
```

- [ ] **Step 9: Verify and commit**

Run: `make test && bash tests/smoke_host.sh`
Expected: both pass.

```bash
git add src/config.c tests/test_config.c config/koboy.ini
git commit -m "fix: config hardening -- blank values, trailing newline, ini docs

Closes follow-ups #6, #8, #9 and #13.

#8: as_bool treated everything except false/0 as true, INCLUDING the
empty string, so a blanked 'grab_input = ' silently turned the grab on.
An empty value now leaves the default alone.

#9: an ini with no trailing newline had the calibration block
concatenated onto its last line. Harmless only because config_load
truncates at the resulting '#', but it rewrote an unrelated line.

#6: the preservation test seeded a key literally named 'old key_a', which
the exact strcmp filter never matched -- so the test passed whether or
not the filter existed. It now seeds a real key_a line.

Three mutants recorded, one per fix."
```

---

### Task 8: The MENU zone

Closes follow-ups **#1** (the shipped `dpad_mode = cross` has no test for the
behaviour that distinguishes it) and **#2** (`flip_x`/`flip_y` are never
exercised). Both are input-geometry gaps, and this task is the one that touches
input geometry.

**Files:**
- Modify: `src/koboy.h` (`koboy_layout` gains four fields)
- Modify: `src/config.c` (layout defaults, ini keys)
- Modify: `src/chrome.c` (draw the MENU button, extend `chrome_controls_top`)
- Modify: `src/input.h`, `src/input.c` (`input_take_menu_request`)
- Modify: `tests/test_input_touch.c` (#1 and #2)
- Modify: `tests/test_chrome.c` (MENU zone must be inside `chrome_controls_top`)
- Delete: `tests/golden/chrome_*.pgm` (regenerate — the faceplate changed)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `koboy_layout.menu_cx, menu_cy, menu_w, menu_h` (permille)
  - `bool input_take_menu_request(koboy_input *in)` — true once per tap, then clears

- [ ] **Step 1: Write the failing test for the cross/relative distinction (#1)**

Append to `tests/test_input_touch.c` inside `TEST_MAIN({ ... })`:

```c
    /* #1: dpad_mode = cross is the SHIPPED DEFAULT and the behaviour that
       distinguishes it from RELATIVE had no coverage, because every existing
       touch test lands on the pad centre -- where the two modes are
       identical by construction.

       The distinction: CROSS derives direction from the drawn cross's fixed
       centre, so a tap anywhere in the pad steers. RELATIVE sets its origin at
       the touch point, so the same tap steers nowhere until the finger drags.
       That difference is why the d-pad was unusable in relative mode on the
       device: the chrome draws an absolute cross and users press its arms. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680);

        const int W = p.panel_w, H = p.panel_h;
        int dcx = c.layout.dpad_cx * W / 1000;
        int dcy = c.layout.dpad_cy * H / 1000;
        int dr  = c.layout.dpad_r  * W / 1000;

        /* Well past the deadzone, well inside the pad: the right-hand arm. */
        int off = c.dpad_deadzone + c.dpad_hysteresis + 20;
        CHECK(off < dr);
        int tx = dcx + off, ty = dcy;

        c.dpad_mode = KOBOY_DPAD_CROSS;
        koboy_input *cross = input_create(&c, &p);
        CHECK(cross != NULL);
        input_set_touch_transform(cross, W - 1, H - 1, false, false, false);
        CHECK_EQ_INT(touch_probe(cross, tx, ty), KOBOY_BTN_RIGHT);
        input_destroy(cross);

        c.dpad_mode = KOBOY_DPAD_RELATIVE;
        koboy_input *rel = input_create(&c, &p);
        CHECK(rel != NULL);
        input_set_touch_transform(rel, W - 1, H - 1, false, false, false);
        /* Same coordinate, and RELATIVE must report NO direction: the origin
           is the touch itself, so displacement is zero. */
        CHECK_EQ_INT(touch_probe(rel, tx, ty), 0);
        input_destroy(rel);
    }

    /* #2: flip_x / flip_y are wired to real per-device probe data in
       platform_kobo.c but no caller in the test suite ever passes true, so any
       Kobo needing a mirrored touch axis depends on an untested path.
       Irrelevant on the verified Libra 2, load-bearing on hardware nobody has
       tried. */
    {
        koboy_config c; config_defaults(&c);
        c.dpad_mode = KOBOY_DPAD_CROSS;
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680);
        const int W = p.panel_w, H = p.panel_h;

        int acx = c.layout.a_cx * W / 1000;
        int acy = c.layout.a_cy * H / 1000;

        /* Unflipped: touching A's centre presses A. */
        koboy_input *plain = input_create(&c, &p);
        CHECK(plain != NULL);
        input_set_touch_transform(plain, W - 1, H - 1, false, false, false);
        CHECK_EQ_INT(touch_probe(plain, acx, acy) & KOBOY_BTN_A, KOBOY_BTN_A);
        input_destroy(plain);

        /* flip_x: the MIRRORED raw coordinate must now land on A, and A's own
           coordinate must not. */
        koboy_input *fx = input_create(&c, &p);
        CHECK(fx != NULL);
        input_set_touch_transform(fx, W - 1, H - 1, false, true, false);
        CHECK_EQ_INT(touch_probe(fx, W - 1 - acx, acy) & KOBOY_BTN_A, KOBOY_BTN_A);
        CHECK_EQ_INT(touch_probe(fx, acx, acy) & KOBOY_BTN_A, 0);
        input_destroy(fx);

        /* flip_y likewise. */
        koboy_input *fy = input_create(&c, &p);
        CHECK(fy != NULL);
        input_set_touch_transform(fy, W - 1, H - 1, false, false, true);
        CHECK_EQ_INT(touch_probe(fy, acx, H - 1 - acy) & KOBOY_BTN_A, KOBOY_BTN_A);
        input_destroy(fy);
    }

    /* The MENU zone: a tap reports once and then clears, and it is NOT a
       joypad bit. There is no libretro button for "menu", and borrowing an
       unused bit would forward it straight to the core. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        config_resolve_profile(&p, &c, 1264, 1680);
        const int W = p.panel_w, H = p.panel_h;

        koboy_input *in = input_create(&c, &p);
        CHECK(in != NULL);
        input_set_touch_transform(in, W - 1, H - 1, false, false, false);

        CHECK_EQ_INT(input_take_menu_request(in), 0);

        int mx = c.layout.menu_cx * W / 1000;
        int my = c.layout.menu_cy * H / 1000;
        uint16_t bits = touch_probe(in, mx, my);
        CHECK_EQ_INT(bits, 0);                        /* no joypad bit */
        CHECK_EQ_INT(input_take_menu_request(in), 1); /* latched once */
        CHECK_EQ_INT(input_take_menu_request(in), 0); /* and cleared */

        input_destroy(in);
    }
```

`touch_probe` already exists in `tests/test_chrome.c`; copy it into
`tests/test_input_touch.c` if that file does not have an equivalent, keeping
its comment about requiring an identity touch transform.

- [ ] **Step 2: Write the failing test for the geometry contract**

Append to `tests/test_chrome.c` inside `TEST_MAIN({ ... })`:

```c
    /* The MENU zone is a LIVE TOUCH ZONE, so chrome_controls_top must account
       for it. That function's contract is "the topmost row any drawn control
       or live touch zone occupies", and it exists because a scale = 0
       auto-fitted rect once swallowed the A button while its touch zone stayed
       live underneath -- tapping the lower playfield pressed A. A new zone the
       function does not know about reintroduces exactly that. */
    {
        koboy_config c; config_defaults(&c);
        const int W = 1264, H = 1680;
        int top = chrome_controls_top(&c.layout, W, H);
        int menu_top = (c.layout.menu_cy * H / 1000) - (c.layout.menu_h * H / 1000) / 2;
        CHECK(top <= menu_top);
    }
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `make build/test_input_touch build/test_chrome`
Expected: FAIL — `'koboy_layout' has no member named 'menu_cx'` and
`implicit declaration of function 'input_take_menu_request'`.

- [ ] **Step 4: Add the layout fields**

In `src/koboy.h`, extend `koboy_layout`:

```c
    int menu_cx, menu_cy, menu_w, menu_h;
```

- [ ] **Step 5: Add the defaults and the ini keys**

In `src/config.c`'s `config_defaults`, extend the layout initialiser:

```c
                       .select_cx = 390, .select_cy = 920, .select_w = 200, .select_h = 55,
                       /* The band between the game rect's bottom and the top of
                          the controls. On the verified Libra 2 (1264x1680) the
                          rect ends at y=804 and chrome_controls_top returned
                          1018 before this zone existed, so 540 permille (y 830)
                          with a 55 permille height (y 830-984) is clear of
                          both with room either side. */
                       .menu_cx = 500, .menu_cy = 540, .menu_w = 200, .menu_h = 55 };
```

In `config_load`'s dispatch:

```c
        else if (!strcmp(k, "menu_cx"))          c->layout.menu_cx = atoi(v);
        else if (!strcmp(k, "menu_cy"))          c->layout.menu_cy = atoi(v);
        else if (!strcmp(k, "menu_w"))           c->layout.menu_w  = atoi(v);
        else if (!strcmp(k, "menu_h"))           c->layout.menu_h  = atoi(v);
```

- [ ] **Step 6: Extend `chrome_controls_top` and draw the button**

In `src/chrome.c`, add to the `min2` chain in `chrome_controls_top`, keeping the
file's rule that every term is the exact expression the corresponding draw call
uses for its top edge:

```c
    top = min2(top, perm(l->menu_cy, H) - perm(l->menu_h, H) / 2);
```

At the end of `chrome_render`, after the Start and Select pills:

```c
    /* MENU. Drawn, not hidden behind a gesture: the drawn UI is the part
       people trust, and v1 already learned that the input model has to match
       the drawing -- a relative thumb-pad under a drawn absolute cross was
       unusable. Power still means quit, so a menu that fails to draw can never
       trap the user on a device where a stuck app looks like a brick. */
    box(fb, stride, W, H, perm(l->menu_cx, W), perm(l->menu_cy, H),
        perm(l->menu_w, W), perm(l->menu_h, H), MID);
    frame(fb, stride, W, H,
          perm(l->menu_cx, W) - perm(l->menu_w, W) / 2,
          perm(l->menu_cy, H) - perm(l->menu_h, H) / 2,
          perm(l->menu_w, W), perm(l->menu_h, H), 2, INK);
```

The label is added in Task 12, which is where `text.c` is wired into `chrome.c`.

- [ ] **Step 7: Add the menu latch to `input.c`**

In `src/input.h`, after `input_state`:

```c
/* True exactly once per MENU tap, then clears.

   Deliberately NOT a joypad bit: there is no libretro button for "menu", and
   borrowing an unused RETRO_DEVICE_ID_JOYPAD_* bit would forward every menu
   tap straight into the running game. */
bool input_take_menu_request(koboy_input *in);
```

In `src/input.c`, add to `struct koboy_input`:

```c
    bool menu_latched;      /* set on a MENU tap, cleared by the taker */
    bool menu_touching;     /* edge state, so a held finger latches once */
```

In `recompute()`, after the A/B/Start/Select loop and before `in->st.buttons = b;`:

```c
    /* MENU is edge triggered for the same reason ui.c is: a held finger would
       otherwise re-open the menu on every poll. */
    bool menu_now = false;
    for (int s = 0; s < KOBOY_MAX_TOUCH; s++) {
        if (!in->st.touch[s].down) continue;
        if (in->pad_active && s == in->pad_slot) continue;
        if (in_rect(in->st.touch[s].x, in->st.touch[s].y,
                    perm(l->menu_cx, W), perm(l->menu_cy, H),
                    perm(l->menu_w, W), perm(l->menu_h, H)))
            menu_now = true;
    }
    if (menu_now && !in->menu_touching) in->menu_latched = true;
    in->menu_touching = menu_now;
```

And at the end of the file:

```c
bool input_take_menu_request(koboy_input *in)
{
    bool v = in->menu_latched;
    in->menu_latched = false;
    return v;
}
```

- [ ] **Step 8: Regenerate the chrome goldens**

The faceplate gained a button, so every golden image is stale. Review the change
before accepting it — `pgm_compare_golden` never auto-updates an existing golden
precisely so this is a deliberate act.

```bash
rm -f tests/golden/chrome_*.pgm
KOBOY_GOLDEN_UPDATE=1 make build/test_chrome && KOBOY_GOLDEN_UPDATE=1 ./build/test_chrome
```

Convert one to PNG and look at it before committing:
`python3 -c "import sys" ` — or simply open `tests/golden/chrome_1264x1680.pgm`
in an image viewer. Confirm the MENU box sits between the game rect and the
d-pad, and that nothing moved that should not have.

- [ ] **Step 9: Run all tests**

Run: `make test`
Expected: PASS — including the new `#1`, `#2` and MENU checks.

- [ ] **Step 10: Verify each new test is real (mutants)**

1. **#1** — in `input.c`'s `recompute`, delete the line
   `if (in->cfg.dpad_mode == KOBOY_DPAD_CROSS) { ox = dcx; oy = dcy; }`.
   Expected: the CROSS probe returns 0 instead of `KOBOY_BTN_RIGHT` and FAILS.
   This is the exact regression follow-up #1 says is untested. Revert.
2. **#2** — delete `if (in->flip_x) px = in->prof.panel_w - 1 - px;`.
   Expected: the `flip_x` checks FAIL. Revert.
3. **MENU latch** — change `if (menu_now && !in->menu_touching)` to
   `if (menu_now)`. Expected: the "and cleared" check still passes but a second
   probe latches again; add nothing — instead change `input_take_menu_request`
   to not clear the flag and confirm the third check FAILS. Revert.
4. **`controls_top`** — remove the `menu_cy` term from the `min2` chain.
   Expected: the geometry check in `test_chrome.c` FAILS. Revert.

Record all four outputs.

- [ ] **Step 11: Commit**

```bash
git add src/koboy.h src/config.c src/chrome.c src/input.h src/input.c \
        tests/test_input_touch.c tests/test_chrome.c tests/golden
git commit -m "feat: a drawn MENU zone, and the input tests v1 deferred

The menu is opened by a drawn button, not a gesture: the drawn UI is the
part people trust, and v1 already learned the input model has to match the
drawing. Power still means quit, so a menu that fails to draw can never
trap the user.

MENU is not a joypad bit -- there is no libretro button for it, and
borrowing an unused one would forward every menu tap into the game. It is
a latched edge-triggered flag instead.

Closes follow-up #1: dpad_mode = cross is the shipped default and the
behaviour distinguishing it from RELATIVE had no coverage, because every
existing touch test lands on the pad centre where the two are identical.
Closes #2: flip_x/flip_y are wired to real probe data and were never
exercised.

Four mutants recorded, including the one that matters most -- removing the
cross-mode origin override makes an off-centre tap steer nowhere, which is
exactly the device failure that made cross the default."
```

---

### Task 9: Core lifecycle — unload, and optional serialisation

Closes follow-ups **#7** (stub-core observation flags) and **#11** (`xdlsym`'s
error omits the `.so` path).

**Files:**
- Modify: `src/core.h`, `src/core.c`
- Modify: `tests/stub_core.c`
- Modify: `tests/test_core.c`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `bool core_unload_rom(koboy_core *c)`
  - `size_t core_state_size(koboy_core *c)` — 0 means the core cannot serialise
  - `bool core_state_save(koboy_core *c, void *buf, size_t n)`
  - `bool core_state_load(koboy_core *c, const void *buf, size_t n)`
  - `bool core_reset(koboy_core *c)`
  - stub core exports `stub_observed_unload`, `stub_observed_reset`,
    `stub_serialize_calls` as dlsym-able symbols

- [ ] **Step 1: Write the failing test**

Append to `tests/test_core.c` inside `TEST_MAIN({ ... })`, after the existing
load succeeds:

```c
    /* #7: the stub's observation flags were only readable under gdb, so the
       assertions they were written for were never actually made. Exported as
       real symbols, they become real checks. */
    {
        void *so = dlopen("build/stub_core.so", RTLD_NOW);
        CHECK(so != NULL);
        int *unloaded = (int *)dlsym(so, "stub_observed_unload");
        int *was_reset = (int *)dlsym(so, "stub_observed_reset");
        int *ser_calls = (int *)dlsym(so, "stub_serialize_calls");
        CHECK(unloaded != NULL);
        CHECK(was_reset != NULL);
        CHECK(ser_calls != NULL);
    }

    /* Save states round-trip through the core. */
    {
        size_t n = core_state_size(core);
        CHECK(n > 0);
        uint8_t *blob = malloc(n);
        CHECK(blob != NULL);
        CHECK_EQ_INT(core_state_save(core, blob, n), 1);
        CHECK_EQ_INT(core_state_load(core, blob, n), 1);

        /* A short buffer is refused rather than truncated: handing a core a
           partial state is how a running game gets corrupted. */
        CHECK_EQ_INT(core_state_save(core, blob, n - 1), 0);
        CHECK_EQ_INT(core_state_load(core, blob, n - 1), 0);
        free(blob);
    }

    CHECK_EQ_INT(core_reset(core), 1);

    /* Unload, then load a different ROM through the SAME handle. dlclose is
       never called mid-session; retro_unload_game plus retro_load_game is the
       libretro-sanctioned way and avoids cycling the shared object. */
    CHECK_EQ_INT(core_unload_rom(core), 1);
    CHECK_EQ_INT(core_load_rom(core, rom_path, err, sizeof err), 1);
```

Ensure `tests/test_core.c` includes `<dlfcn.h>` and `<stdlib.h>`, and that
`rom_path` names the temporary ROM the test already creates.

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_core && ./build/test_core`
Expected: FAIL — `implicit declaration of function 'core_state_size'` etc.

- [ ] **Step 3: Declare the new core API**

In `src/core.h`, before `core_close`:

```c
/* Unloads the current game but keeps the shared object open and initialised,
   so another ROM can be loaded through the same handle. dlclose/dlopen cycling
   of a C++ core mid-session is avoidable, so it is avoided. */
bool core_unload_rom(koboy_core *c);

/* retro_reset. */
bool core_reset(koboy_core *c);

/* Save-state support. core_state_size returns 0 when the core does not export
   the serialisation symbols -- a capability answer, not an error. The menu
   greys the entries out; the game still plays. */
size_t core_state_size(koboy_core *c);

/* Both refuse a buffer shorter than core_state_size rather than truncating:
   handing a core a partial state corrupts a running game. */
bool   core_state_save(koboy_core *c, void *buf, size_t n);
bool   core_state_load(koboy_core *c, const void *buf, size_t n);
```

- [ ] **Step 4: Bind the symbols optionally and implement**

In `src/core.c`, add to the core struct:

```c
    size_t (*serialize_size)(void);
    bool   (*serialize)(void *, size_t);
    bool   (*unserialize)(const void *, size_t);
    bool    game_loaded;
```

Beside the existing hard `BIND` macro, add:

```c
/* Optional: a missing symbol is a capability answer, not a fatal error. The
   test stub is the immediate reason, but the rule is general -- refusing to
   start because a core cannot serialise would trade playing the game for a
   feature the user did not ask for. */
#define BIND_OPT(field, name) \
    do { *(void **)&c->field = dlsym(so, name); } while (0)
```

After the existing `BIND(reset, "retro_reset");`:

```c
    BIND_OPT(serialize_size, "retro_serialize_size");
    BIND_OPT(serialize,      "retro_serialize");
    BIND_OPT(unserialize,    "retro_unserialize");
```

Then the implementations:

```c
bool core_unload_rom(koboy_core *c)
{
    if (!c || !c->game_loaded) return false;
    c->unload_game();
    c->game_loaded = false;
    return true;
}

bool core_reset(koboy_core *c)
{
    if (!c || !c->reset) return false;
    c->reset();
    return true;
}

size_t core_state_size(koboy_core *c)
{
    /* All three are required together: a core exporting only some of them
       cannot round-trip, and reporting a non-zero size would offer the user a
       Save that silently cannot be loaded. */
    if (!c || !c->serialize_size || !c->serialize || !c->unserialize) return 0;
    return c->serialize_size();
}

bool core_state_save(koboy_core *c, void *buf, size_t n)
{
    size_t need = core_state_size(c);
    if (!need || n < need || !buf) return false;
    return c->serialize(buf, need);
}

bool core_state_load(koboy_core *c, const void *buf, size_t n)
{
    size_t need = core_state_size(c);
    if (!need || n < need || !buf) return false;
    return c->unserialize(buf, need);
}
```

Set `c->game_loaded = true;` at the end of the successful path in
`core_load_rom`, and make `core_close` call `core_unload_rom(c)` instead of
calling `unload_game()` unconditionally, so a double unload is impossible.

- [ ] **Step 5: Fix `xdlsym`'s error message (#11)**

In `src/core.c`, `xdlsym` reports a missing symbol without naming the `.so`,
while its sibling `dlopen` error does include it. On a device where a photo of
the panel is the only diagnostic, that path is the useful half. Pass the path in
and include it:

```c
static void *xdlsym(void *so, const char *name, const char *so_path,
                    char *err, size_t errlen)
{
    void *p = dlsym(so, name);
    if (!p)
        snprintf(err, errlen, "core %s is missing %s", so_path, name);
    return p;
}
```

Update the `BIND` macro and every call site to pass `so_path`.

- [ ] **Step 6: Export the stub's observation flags (#7)**

In `tests/stub_core.c`, change the internal observation counters to exported
symbols with external linkage and add serialisation:

```c
/* Exported, not static: the test used to inspect these under gdb, which meant
   the assertions they existed for were never actually made. dlsym-able flags
   are real assertions. */
int stub_observed_unload = 0;
int stub_observed_reset  = 0;
int stub_serialize_calls = 0;

#define STUB_STATE_BYTES 128
static unsigned char stub_state[STUB_STATE_BYTES];

void retro_unload_game(void) { stub_observed_unload++; }
void retro_reset(void)       { stub_observed_reset++; }

size_t retro_serialize_size(void) { return STUB_STATE_BYTES; }

bool retro_serialize(void *data, size_t size)
{
    if (size < STUB_STATE_BYTES) return false;
    stub_serialize_calls++;
    memcpy(data, stub_state, STUB_STATE_BYTES);
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    if (size < STUB_STATE_BYTES) return false;
    memcpy(stub_state, data, STUB_STATE_BYTES);
    return true;
}
```

Keep whatever `retro_unload_game`/`retro_reset` already did and add the counter
increment; do not drop existing behaviour.

- [ ] **Step 7: Run the test to verify it passes**

Run: `make build/stub_core.so build/test_core && ./build/test_core`
Expected: PASS.

- [ ] **Step 8: Verify the short-buffer refusal is real (mutant)**

In `core_state_save`, change `n < need` to `n < 0`. Rebuild and run.
Expected: `CHECK_EQ_INT(core_state_save(core, blob, n - 1), 0)` FAILS.
Repeat for `core_state_load`. Revert both. Record the output.

This mutant matters: without the check the core writes `need` bytes into a
buffer of `need - 1`, which is a heap overflow in the save path.

- [ ] **Step 9: Verify optional binding is real (mutant)**

Change `BIND_OPT` to the hard `BIND`, then temporarily remove
`retro_serialize_size` from `tests/stub_core.c`.
Expected: `core_open` now FAILS outright instead of reporting
`core_state_size() == 0` — i.e. a core that merely lacks save states can no
longer play games at all. Revert both. Record the output.

- [ ] **Step 10: Commit**

```bash
git add src/core.h src/core.c tests/stub_core.c tests/test_core.c
git commit -m "feat: core unload, reset and optional save-state serialisation

retro_unload_game plus retro_load_game through the same handle, so
switching ROMs never dlclose/dlopens a C++ core mid-session.

The serialisation trio binds OPTIONALLY. A core lacking it must still play
games -- refusing to start would trade the game for a feature nobody asked
for. core_state_size returns 0 and the menu greys the entries out.

Both state calls refuse a short buffer rather than truncating; without
that check the core writes need bytes into need-1, which is a heap
overflow in the save path.

Closes #11 (xdlsym's error now names the .so, like its dlopen sibling --
on a device where a photo of the panel is the only diagnostic, that path
is the useful half) and #7 (the stub's observation flags were gdb-only,
so the assertions they existed for were never made)."
```

---

### Task 10: Save-state storage

**Files:**
- Create: `src/safefile.h`, `src/safefile.c`
- Create: `src/state.h`, `src/state.c`
- Create: `tests/test_state.c`
- Modify: `src/sram.c` (use `safefile_write` / `safefile_read_exact`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `bool safefile_write(const char *path, const void *src, size_t len)`
  - `bool safefile_read_exact(const char *path, void *dst, size_t len)`
  - `#define KOBOY_STATE_SLOTS 3`
  - `void state_path(char *out, size_t n, const char *save_dir, const char *rom_path, int slot)`
  - `bool state_exists(const char *save_dir, const char *rom_path, int slot)`
  - `void state_slot_label(char *out, size_t n, const char *save_dir, const char *rom_path, int slot)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_state.c`:

```c
#include "test.h"
#include "safefile.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST_MAIN({
    char dir[] = "/tmp/koboy_state_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);

    char rom[512];
    snprintf(rom, sizeof rom, "%s/ZELDA.gb", dir);

    /* Slot paths are derived from the ROM stem, like .srm, so two games never
       share a slot. */
    char p1[512], p2[512];
    state_path(p1, sizeof p1, dir, rom, 1);
    state_path(p2, sizeof p2, dir, rom, 2);
    CHECK(strcmp(p1, p2) != 0);
    CHECK(strstr(p1, "ZELDA") != NULL);
    CHECK(strstr(p1, ".st1") != NULL);

    /* Out-of-range slots write an empty path rather than a surprising file. */
    char bad[512];
    state_path(bad, sizeof bad, dir, rom, 0);
    CHECK_EQ_INT(bad[0], 0);
    state_path(bad, sizeof bad, dir, rom, KOBOY_STATE_SLOTS + 1);
    CHECK_EQ_INT(bad[0], 0);

    CHECK_EQ_INT(state_exists(dir, rom, 1), 0);

    /* safefile_write is atomic: temp file plus rename, so a kill mid-write
       cannot corrupt an existing file. */
    unsigned char blob[64];
    for (int i = 0; i < 64; i++) blob[i] = (unsigned char)i;
    CHECK_EQ_INT(safefile_write(p1, blob, sizeof blob), 1);
    CHECK_EQ_INT(state_exists(dir, rom, 1), 1);

    /* No .tmp is left behind. */
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", p1);
    FILE *leftover = fopen(tmp, "rb");
    CHECK(leftover == NULL);
    if (leftover) fclose(leftover);

    unsigned char back[64];
    memset(back, 0xAA, sizeof back);
    CHECK_EQ_INT(safefile_read_exact(p1, back, sizeof back), 1);
    CHECK_EQ_INT(memcmp(back, blob, sizeof blob), 0);

    /* ALL OR NOTHING. A file shorter than the buffer must leave the buffer
       UNTOUCHED, because in real use that buffer is the core's live state.
       sram.c's comment records what happens otherwise: the previous version
       read straight into live memory and only then reported failure, so
       loading a truncated save destroyed it. A truncated state does the same
       to a running game. */
    {
        char shortp[512];
        snprintf(shortp, sizeof shortp, "%s/short.bin", dir);
        FILE *f = fopen(shortp, "wb");
        CHECK(f != NULL);
        fwrite(blob, 1, 10, f);
        fclose(f);

        unsigned char guard[64];
        memset(guard, 0x5A, sizeof guard);
        CHECK_EQ_INT(safefile_read_exact(shortp, guard, sizeof guard), 0);
        int untouched = 1;
        for (int i = 0; i < 64; i++) if (guard[i] != 0x5A) untouched = 0;
        CHECK_EQ_INT(untouched, 1);
    }

    /* A LONGER file is accepted, reading the first len bytes -- matching what
       sram_load does, because a mismatch there means a different cartridge or
       trailing data (RTC state, say), and refusing would be a new failure mode
       rather than a fix for this one. */
    {
        char longp[512];
        snprintf(longp, sizeof longp, "%s/long.bin", dir);
        FILE *f = fopen(longp, "wb");
        CHECK(f != NULL);
        fwrite(blob, 1, sizeof blob, f);
        fwrite(blob, 1, sizeof blob, f);
        fclose(f);
        unsigned char got[64];
        CHECK_EQ_INT(safefile_read_exact(longp, got, sizeof got), 1);
        CHECK_EQ_INT(memcmp(got, blob, sizeof blob), 0);
    }

    /* A missing file is a clean false, not a crash. */
    CHECK_EQ_INT(safefile_read_exact("/nonexistent/koboy/x", back, sizeof back), 0);

    /* Labels tell the user which slot they are about to overwrite, which is
       most of the value of having slots at all. */
    char label[64];
    state_slot_label(label, sizeof label, dir, rom, 1);
    CHECK(strstr(label, "1") != NULL);
    state_slot_label(label, sizeof label, dir, rom, 2);
    CHECK(strstr(label, "EMPTY") != NULL);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "NOTE: cleanup failed\n");
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_state`
Expected: FAIL — `fatal error: safefile.h: No such file or directory`

- [ ] **Step 3: Write `safefile`**

Create `src/safefile.h`:

```c
#ifndef KOBOY_SAFEFILE_H
#define KOBOY_SAFEFILE_H
#include <stdbool.h>
#include <stddef.h>

/* The two file operations koboy uses for user data, extracted from sram.c so
   save states inherit its discipline rather than reinventing a weaker version.
   Both save files and save states are the only data koboy owns, and e-readers
   get killed unceremoniously. */

/* Temp file, fsync, rename. A kill mid-write cannot corrupt an existing file. */
bool safefile_write(const char *path, const void *src, size_t len);

/* Fills dst only if the whole of it can be filled. NOTHING is written to dst
   otherwise -- see the long note in the implementation; this property is the
   entire point. A file LONGER than len is accepted, reading the first len
   bytes. */
bool safefile_read_exact(const char *path, void *dst, size_t len);
#endif
```

Create `src/safefile.c` by moving the bodies of `sram_save` and `sram_load` from
`src/sram.c` verbatim — **including their comments**, which record why the
all-or-nothing property exists — renaming them and generalising the parameter
names:

```c
#define _POSIX_C_SOURCE 200809L
#include "safefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Temp file plus rename, so a kill mid-write cannot corrupt an existing file. */
bool safefile_write(const char *path, const void *src, size_t len)
{
    if (!len) return true;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(src, 1, len, f) == len;
    if (ok) ok = (fflush(f) == 0);
    if (ok) ok = (fsync(fileno(f)) == 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }  /* best-effort cleanup */
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

/* ALL OR NOTHING, and the temporary buffer is the whole point: `dst` is live
   memory -- the core's save RAM, or the state a running game is about to be
   restored from. An earlier version of this logic fread() straight into it and
   only then reported failure, so a truncated file left the destination as a mix
   of partial file and prior contents. For SRAM the periodic flush ten seconds
   later then wrote that hybrid back over the user's only save: loading the save
   destroyed it. Nothing may touch `dst` unless the whole of it can be filled.
   A file LONGER than len is still accepted, reading the first len bytes, which
   is what the SRAM path always did: a mismatch there means a different
   cartridge or a format with trailing data (RTC state, for instance), and
   refusing to load would be a new failure mode rather than a fix for this one. */
bool safefile_read_exact(const char *path, void *dst, size_t len)
{
    if (!dst || !len) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char *tmp = malloc(len);
    if (!tmp) { fclose(f); return false; }
    size_t got = fread(tmp, 1, len, f);
    fclose(f);
    bool ok = (got == len);
    if (ok) memcpy(dst, tmp, len);
    free(tmp);
    return ok;
}
```

Then reduce `src/sram.c`'s two functions to thin wrappers, keeping
`sram_path_for_rom` unchanged:

```c
#include "safefile.h"

bool sram_save(const char *path, const uint8_t *src, size_t len)
{ return safefile_write(path, src, len); }

bool sram_load(const char *path, uint8_t *dst, size_t len)
{ return safefile_read_exact(path, dst, len); }
```

- [ ] **Step 4: Write `state`**

Create `src/state.h`:

```c
#ifndef KOBOY_STATE_H
#define KOBOY_STATE_H
#include <stdbool.h>
#include <stddef.h>

#define KOBOY_STATE_SLOTS 3

/* <save_dir>/<rom stem>.st<N>, derived the same way .srm is, so two games never
   share a slot. Slots are 1-based; an out-of-range slot writes "". */
void state_path(char *out, size_t n, const char *save_dir,
                const char *rom_path, int slot);

bool state_exists(const char *save_dir, const char *rom_path, int slot);

/* A menu row: "SLOT 1 - SAVED" or "SLOT 2 - EMPTY". Knowing which slot you are
   about to overwrite is most of the value of having slots. */
void state_slot_label(char *out, size_t n, const char *save_dir,
                      const char *rom_path, int slot);
#endif
```

Create `src/state.c`:

```c
#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include "sram.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void state_path(char *out, size_t n, const char *save_dir,
                const char *rom_path, int slot)
{
    if (!out || n == 0) return;
    /* Live bound: the slot index comes from a touch on a menu row, so a stale
       page after a ROM switch can reach this with a nonsense value. Writing ""
       makes every caller's fopen fail cleanly instead of creating a file with a
       surprising name. */
    if (slot < 1 || slot > KOBOY_STATE_SLOTS) { out[0] = 0; return; }

    /* Reuse the .srm stem logic rather than duplicating it, so the two kinds of
       save file can never disagree about what a ROM is called. */
    char srm[512];
    sram_path_for_rom(srm, sizeof srm, save_dir, rom_path);
    char *dot = strrchr(srm, '.');
    if (dot) *dot = 0;
    snprintf(out, n, "%s.st%d", srm, slot);
}

bool state_exists(const char *save_dir, const char *rom_path, int slot)
{
    char p[512];
    state_path(p, sizeof p, save_dir, rom_path, slot);
    if (!p[0]) return false;
    return access(p, R_OK) == 0;
}

void state_slot_label(char *out, size_t n, const char *save_dir,
                      const char *rom_path, int slot)
{
    if (!out || n == 0) return;
    snprintf(out, n, "SLOT %d - %s", slot,
             state_exists(save_dir, rom_path, slot) ? "SAVED" : "EMPTY");
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/test_state && ./build/test_state && make build/test_sram && ./build/test_sram`
Expected: both PASS — the existing SRAM tests must be untouched by the
extraction, which is the point of moving the bodies verbatim.

- [ ] **Step 6: Verify the all-or-nothing property is real (mutant)**

In `safefile_read_exact`, replace the temp buffer with a direct read:

```c
    size_t got = fread(dst, 1, len, f);
    fclose(f);
    return got == len;
```

Rebuild and run.
Expected: the `untouched` check FAILS — the guard bytes were overwritten by a
partial read. This is the exact bug `sram.c`'s comment describes, reproduced.
Revert. Record the output.

- [ ] **Step 7: Verify the slot bound is real (mutant)**

Delete the `if (slot < 1 || slot > KOBOY_STATE_SLOTS)` guard.
Expected: the `bad[0] == 0` checks FAIL. Revert. Record the output.

- [ ] **Step 8: Commit**

```bash
git add src/safefile.h src/safefile.c src/state.h src/state.c \
        src/sram.c tests/test_state.c
git commit -m "feat: save-state slots, sharing sram.c's hard-won discipline

safefile extracts sram.c's two file operations verbatim, comments and
all, so save states inherit the all-or-nothing load rather than
reinventing a weaker version. A truncated state loaded into a running
game destroys it the same way a truncated .srm destroyed the save it was
loading.

Mutant: reading straight into the destination instead of a temp buffer
makes the guard bytes change on a short file -- the original bug,
reproduced on demand."
```

---

### Task 11: `MODE_MENU`

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Consumes: `ui_list_*`, `input_take_menu_request`, `core_unload_rom`,
  `core_reset`, `core_state_*`, `state_path`, `state_slot_label`,
  `safefile_write`, `safefile_read_exact`, `romlist_*`, `video_invalidate`.
- Produces: nothing new; this task wires existing pieces together.

- [ ] **Step 1: Extract the ROM-loading sequence into a function**

The emulator loop needs to load a ROM more than once now. In `src/main.c`, above
`main()`:

```c
/* Everything that must happen when a ROM becomes the current game, in the one
   order that is safe.

   Three hazards live here, all of them silent if got wrong:
     - core_sram() is re-fetched every time. The pointer belongs to the core's
       freshly loaded cartridge; caching it across unload/load is a
       use-after-free waiting for a second game.
     - The OUTGOING game's SRAM is flushed by the caller BEFORE unload, never
       after: retro_unload_game takes the buffer, and its last minutes with it.
     - sram_writeback stays false for the session when a save file exists but
       could not be read whole, so nothing is written back over it. */
typedef struct {
    char     path[512];        /* .srm path for the current rom */
    uint8_t *mem;
    size_t   len;
    bool     writeback;
} koboy_sram_binding;

static bool load_rom_into(koboy_core *core, koboy_config *cfg,
                          koboy_sram_binding *sb, char *err, size_t errlen)
{
    if (!core_load_rom(core, cfg->rom_path, err, errlen)) return false;

    sram_path_for_rom(sb->path, sizeof sb->path, cfg->save_dir, cfg->rom_path);
    sb->len = 0;
    sb->mem = core_sram(core, &sb->len);
    sb->writeback = true;
    return true;
}
```

Replace the existing inline `core_load_rom` + `sram_path_for_rom` + `core_sram`
block in `main()` with a call to `load_rom_into`, keeping the existing
"could not be read whole" handling and its on-panel `fatal` exactly as it is —
that block's comment is load-bearing and must not be lost.

- [ ] **Step 2: Add the menu**

Above `main()`:

```c
enum {
    MENU_SAVE = 0, MENU_LOAD, MENU_RESET, MENU_CHOOSE_ROM, MENU_RESUME, MENU_QUIT,
    MENU_COUNT
};

/* Returns the chosen MENU_* action, or MENU_RESUME if the user backed out.
   `has_states` greys nothing out visually -- the label says so instead, which
   is cheaper on a panel with no colour and no hover. */
static int run_menu(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                    int stride, int pw, int ph, bool has_states,
                    const koboy_input_state *script, int script_n)
{
    const char *items[MENU_COUNT];
    items[MENU_SAVE]        = has_states ? "SAVE STATE" : "SAVE STATE (UNSUPPORTED)";
    items[MENU_LOAD]        = has_states ? "LOAD STATE" : "LOAD STATE (UNSUPPORTED)";
    items[MENU_RESET]       = "RESET GAME";
    items[MENU_CHOOSE_ROM]  = "CHOOSE ROM";
    items[MENU_RESUME]      = "RESUME";
    items[MENU_QUIT]        = "QUIT";

    koboy_ui_list list;
    ui_list_init(&list, "MENU", items, MENU_COUNT,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, script_n);
    if (pick < 0) return MENU_RESUME;
    if ((pick == MENU_SAVE || pick == MENU_LOAD) && !has_states) return MENU_RESUME;
    return pick;
}

/* Returns the chosen slot (1-based), or 0 if the user backed out. */
static int run_slot_picker(koboy_platform *pf, koboy_input *in, uint8_t *panel,
                           int stride, int pw, int ph, const char *title,
                           const char *save_dir, const char *rom_path,
                           const koboy_input_state *script, int script_n)
{
    static char labels[KOBOY_STATE_SLOTS + 1][64];
    const char *items[KOBOY_STATE_SLOTS + 1];
    for (int s = 1; s <= KOBOY_STATE_SLOTS; s++) {
        state_slot_label(labels[s - 1], sizeof labels[s - 1], save_dir, rom_path, s);
        items[s - 1] = labels[s - 1];
    }
    snprintf(labels[KOBOY_STATE_SLOTS], sizeof labels[KOBOY_STATE_SLOTS], "BACK");
    items[KOBOY_STATE_SLOTS] = labels[KOBOY_STATE_SLOTS];

    koboy_ui_list list;
    ui_list_init(&list, title, items, KOBOY_STATE_SLOTS + 1,
                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                 pw - 2 * KOBOY_CHROME_MARGIN, ph - 2 * KOBOY_CHROME_MARGIN);

    int pick = run_list(pf, in, &list, panel, stride, pw, ph, script, script_n);
    if (pick < 0 || pick >= KOBOY_STATE_SLOTS) return 0;
    return pick + 1;
}
```

Add the required includes to `main.c`: `#include "state.h"` and
`#include "safefile.h"`.

- [ ] **Step 3: Open the menu from the emulator loop**

Inside the `while (!g_stop && !pf->should_quit(pf->ctx))` loop, immediately
after `pf->poll_input(pf->ctx, in);`:

```c
        if (input_take_menu_request(in)) {
            size_t ssz = core_state_size(core);
            int act = run_menu(pf, panel, panel_stride, pw, ph, in,
                               ssz > 0, NULL, 0);

            if (act == MENU_SAVE || act == MENU_LOAD) {
                int slot = run_slot_picker(pf, in, panel, panel_stride, pw, ph,
                                           act == MENU_SAVE ? "SAVE TO" : "LOAD FROM",
                                           cfg.save_dir, cfg.rom_path, NULL, 0);
                if (slot) {
                    char sp[512];
                    state_path(sp, sizeof sp, cfg.save_dir, cfg.rom_path, slot);
                    uint8_t *blob = malloc(ssz);
                    if (!blob) {
                        fatal("out of memory for a save state");
                    } else if (act == MENU_SAVE) {
                        if (core_state_save(core, blob, ssz) &&
                            safefile_write(sp, blob, ssz))
                            say("koboy: saved state %d\n", slot);
                        else
                            fatal("could not write\nsave state %d", slot);
                    } else {
                        /* All or nothing: safefile_read_exact leaves blob
                           untouched on a short file, and only a complete blob
                           ever reaches the running core. */
                        if (safefile_read_exact(sp, blob, ssz) &&
                            core_state_load(core, blob, ssz)) {
                            say("koboy: loaded state %d\n", slot);
                            /* The core's cartridge RAM was just rewritten --
                               gambatte's blob includes it -- so the periodic
                               flush will now write that to .srm. Correct, and
                               worth knowing: a state load is indirectly a
                               save-file write. Re-fetch in case the pointer
                               moved. */
                            sb.mem = core_sram(core, &sb.len);
                        } else {
                            fatal("could not load\nsave state %d", slot);
                        }
                    }
                    free(blob);
                }
            } else if (act == MENU_RESET) {
                core_reset(core);
            } else if (act == MENU_QUIT) {
                g_stop = 1;
            } else if (act == MENU_CHOOSE_ROM) {
                /* Flush BEFORE unload: retro_unload_game takes the buffer. */
                if (sb.mem && sb.len && sb.writeback)
                    sram_save(sb.path, sb.mem, sb.len);
                core_unload_rom(core);

                koboy_romlist rl;
                int n = romlist_scan(&rl, cfg.rom_dir);
                int pick = -1;
                if (n > 0) {
                    koboy_ui_list list;
                    ui_list_init(&list, "CHOOSE A GAME", romlist_items(&rl), n,
                                 KOBOY_CHROME_MARGIN, KOBOY_CHROME_MARGIN,
                                 pw - 2 * KOBOY_CHROME_MARGIN,
                                 ph - 2 * KOBOY_CHROME_MARGIN);
                    pick = run_list(pf, in, &list, panel, panel_stride,
                                    pw, ph, NULL, 0);
                }
                if (pick < 0) { g_stop = 1; }
                else {
                    romlist_path(&rl, pick, cfg.rom_path, sizeof cfg.rom_path);
                    char lerr[512];
                    if (!load_rom_into(core, &cfg, &sb, lerr, sizeof lerr)) {
                        fatal("%s", lerr);
                        g_stop = 1;
                    } else if (sb.mem && sb.len &&
                               !sram_load(sb.path, sb.mem, sb.len) &&
                               access(sb.path, F_OK) == 0) {
                        sb.writeback = false;
                        fatal("Save file unreadable.\nStarting fresh.\n"
                              "Saving is OFF this run.");
                    }
                }
            }

            /* Whatever happened, the panel is now showing a menu. */
            redraw_chrome(pf, panel, panel_stride, pw, ph, &prof, &cfg.layout);
            video_invalidate(vid);
            pacer_init(&pace, pf->now_us(pf->ctx), cfg.present_divisor);
            continue;
        }
```

Replace the loop's existing references to `sram`/`sram_len`/`sram_path`/
`sram_writeback` with `sb.mem`/`sb.len`/`sb.path`/`sb.writeback` throughout,
including the periodic flush and the final flush after the loop.

- [ ] **Step 4: Build and exercise the menu on the host**

```bash
make host
mkdir -p /tmp/koboy_roms && : > "/tmp/koboy_roms/AAA TEST.gb"
./build/koboy --core build/stub_core.so --rom "/tmp/koboy_roms/AAA TEST.gb" \
              --panel 1264x1680 --frames 60 2>&1 | tail -5
```
Expected: the run completes and prints `presented=`. The SDL backend lets you
click the drawn MENU box to confirm the menu opens interactively; do that once
by hand without `--frames`.

- [ ] **Step 5: Add a scripted menu run to the smoke test**

Extend `tests/smoke_host.sh` with a run that opens the menu by tapping the MENU
zone and then selects RESUME, asserting the run continues:

```sh
# The menu is reachable only through a live touch, so like the browser it is
# invisible to every --rom test. Tap the MENU box, then RESUME.
romdir="$(mktemp -d)"; : > "$romdir/AAA TEST.gb"
out="$("$KOBOY" --core "$STUB" --rom "$romdir/AAA TEST.gb" \
                --panel 1264x1680 --frames 60 2>&1)"
echo "$out" | grep -q '^presented=' \
    || { echo "FAIL: menu-capable build did not run"; echo "$out"; exit 1; }
rm -rf "$romdir"
echo "ok: menu build runs"
```

Note honestly in the script's comment that this asserts the build runs with the
menu wired in, **not** that the menu was entered — driving `MODE_MENU` from a
script requires the emulator loop to accept `--ui-script`, which is deferred.
Do not claim coverage the test does not provide.

- [ ] **Step 6: Verify and commit**

Run: `make test && bash tests/smoke_host.sh`

```bash
git add src/main.c tests/smoke_host.sh
git commit -m "feat: MODE_MENU -- save states, reset, switch ROM, quit

The three lifecycle hazards are handled explicitly and commented at the
point they bite: core_sram() is re-fetched after every load because the
pointer belongs to the freshly loaded cartridge; the outgoing game's SRAM
is flushed BEFORE unload because retro_unload_game takes the buffer; and
a state load rewrites cartridge RAM, so the periodic flush then writes it
to .srm -- correct, but worth saying out loud.

Power still means quit. The menu's Quit entry is a convenience, not the
only exit, because a menu that fails to draw must never trap the user."
```

---

### Task 12: The DMG faceplate

**Files:**
- Modify: `src/platform_if.h` (optional `battery_percent`)
- Modify: `src/platform_sdl.c`, `src/platform_kobo.c` (implement it)
- Modify: `src/chrome.h`, `src/chrome.c` (the redraw)
- Modify: `tests/test_chrome.c`
- Delete/regenerate: `tests/golden/chrome_*.pgm`

**Interfaces:**
- Consumes: `text_draw`, `text_draw_centred`, `text_measure` (Task 2).
- Produces:
  - `int (*battery_percent)(void *ctx)` on `koboy_platform` — returns 0..100, or
    -1 when unknown
  - `void chrome_render_battery(uint8_t *fb, int stride, const koboy_profile *p, const koboy_layout *l, int percent)`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_chrome.c` inside `TEST_MAIN({ ... })`:

```c
    /* The faceplate must be LABELLED. Before this task A, B, Start and Select
       were four indistinguishable grey shapes. Labels are the difference
       between a faceplate and a set of blobs, and text.c exists so they are
       possible at all. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        const int W = 1264, H = 1680;
        static uint8_t fb2[1264 * 1680];
        config_resolve_profile(&p, &c, W, H);
        memset(fb2, 0x7F, (size_t)W * H);
        chrome_render(fb2, W, &p, &c.layout);

        /* The chrome is drawn with KOBOY_REFRESH_FULL, i.e. GC16 and sixteen
           levels. The four-level ceiling constrains the GAME RECT only, and
           before this task the faceplate used three values out of sixteen. A
           tonal ramp costs nothing at runtime. */
        int distinct = 0;
        int seen[256] = {0};
        for (size_t i = 0; i < (size_t)W * H; i++)
            if (!seen[fb2[i]]) { seen[fb2[i]] = 1; distinct++; }
        CHECK(distinct >= 5);

        /* Still never inside the game rect -- the contract that predates this
           redraw and survives it. */
        int intruded = 0;
        for (int y = p.game_y; y < p.game_y + p.game_h; y++)
            for (int x = p.game_x; x < p.game_x + p.game_w; x++)
                if (fb2[y * W + x] != 0x7F) intruded++;
        CHECK_EQ_INT(intruded, 0);

        CHECK(pgm_compare_golden("chrome_1264x1680", fb2, W, H, W) == 1);
    }

    /* The battery lamp renders from a percentage, and an unknown battery (-1)
       is a valid input rather than a crash: the SDL backend has no battery and
       an unseen Kobo may not expose one either. */
    {
        koboy_config c; config_defaults(&c);
        koboy_profile p;
        const int W = 1264, H = 1680;
        static uint8_t a[1264 * 1680], b[1264 * 1680];
        config_resolve_profile(&p, &c, W, H);

        memset(a, 0xFF, (size_t)W * H);
        chrome_render(a, W, &p, &c.layout);
        chrome_render_battery(a, W, &p, &c.layout, 100);

        memset(b, 0xFF, (size_t)W * H);
        chrome_render(b, W, &p, &c.layout);
        chrome_render_battery(b, W, &p, &c.layout, 5);

        /* Different levels must look different, or the indicator is a lie. */
        CHECK(memcmp(a, b, (size_t)W * H) != 0);

        /* Unknown must not write inside the game rect either. */
        memset(a, 0x7F, (size_t)W * H);
        chrome_render(a, W, &p, &c.layout);
        chrome_render_battery(a, W, &p, &c.layout, -1);
        int intruded = 0;
        for (int y = p.game_y; y < p.game_y + p.game_h; y++)
            for (int x = p.game_x; x < p.game_x + p.game_w; x++)
                if (a[y * W + x] != 0x7F) intruded++;
        CHECK_EQ_INT(intruded, 0);
    }

    /* NO NINTENDO MARKS. This is a public GPLv3 repo; the faceplate is an
       homage to the industrial design and carries none of the word marks.
       Asserted on the source rather than the pixels, because that is where a
       future edit would add one. */
    {
        FILE *f = fopen("src/chrome.c", "rb");
        CHECK(f != NULL);
        static char src[200000];
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        for (size_t i = 0; i < n; i++)
            if (src[i] >= 'a' && src[i] <= 'z') src[i] = (char)(src[i] - 32);
        CHECK(strstr(src, "NINTENDO") == NULL);
        CHECK(strstr(src, "GAME BOY") == NULL);
        CHECK(strstr(src, "GAMEBOY") == NULL);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_chrome && ./build/test_chrome`
Expected: FAIL — `implicit declaration of function 'chrome_render_battery'`,
and once that is stubbed, the `distinct >= 5` check fails because the current
faceplate uses three tones.

- [ ] **Step 3: Add the battery hook to the platform seam**

In `src/platform_if.h`, add to `koboy_platform`:

```c
    /* Device battery, 0..100, or -1 when unknown. Optional: the SDL backend
       has no battery, and an unseen Kobo may not expose one either. Read only
       when the whole panel is already being repainted, so the faceplate keeps
       its zero-per-frame-cost property and needs no timer. */
    int      (*battery_percent)(void *ctx);
```

In `src/platform_sdl.c`, add:

```c
static int sdl_battery_percent(void *ctx) { (void)ctx; return -1; }
```
and assign it in the platform struct.

In `src/platform_kobo.c`:

```c
/* Kobo exposes battery capacity through the standard power-supply class. The
   node name differs by model, so the directory is scanned rather than
   hardcoded -- the same capability-detection rule the rest of the backend
   follows. A missing or unreadable node is -1, not an error. */
static int kobo_battery_percent(void *ctx)
{
    (void)ctx;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    int pct = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity",
                 e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int v = -1;
        if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100) pct = v;
        fclose(f);
        if (pct >= 0) break;
    }
    closedir(d);
    return pct;
}
```
Add `#include <dirent.h>` and assign the function pointer.

- [ ] **Step 4: Redraw the faceplate**

In `src/chrome.h`, add:

```c
/* Draws the battery lamp for `percent` (0..100), or an unfilled lamp when
   percent < 0. Separate from chrome_render because it is the one element with
   a value that changes: it is redrawn whenever the whole panel is already
   being repainted -- startup, menu exit, a chrome restore -- so the faceplate
   keeps its zero-per-frame-cost property and needs no timer. A dedicated timer
   was rejected: on a panel where every refresh is visible, adding a periodic
   one to show a number that changes over hours is a bad trade. */
void chrome_render_battery(uint8_t *fb, int stride, const koboy_profile *p,
                           const koboy_layout *l, int percent);
```

In `src/chrome.c`, add `#include "text.h"` and replace the three-tone palette
with a ramp, keeping the existing names so the diff stays readable:

```c
/* The faceplate is drawn with KOBOY_REFRESH_FULL -- GC16, sixteen levels. The
   four-level ceiling is a constraint on the GAME RECT only, and this file used
   three values out of sixteen. Depth is free here: chrome is drawn once, so
   elaborateness is an authoring question and not a performance one. */
#define BG      0xFF   /* case */
#define CASE_HI 0xEE   /* raised edge */
#define CASE_LO 0xD0   /* recess */
#define MID     0xAA   /* button face */
#define DARK    0x66   /* button shadow, bezel inner */
#define INK     0x00
```

Then extend `chrome_render`, after the existing d-pad / A / B / Start / Select /
MENU drawing:

1. **Asymmetric screen surround.** Replace the uniform `frame(..., 6, INK)`
   with a `DARK` bezel that is `p->game_h / 12` taller below the rect than
   above. The asymmetry is what makes it read as a Game Boy.
2. **Strapline** centred in that lower bezel band, drawn with `text_draw_centred`
   at a small `px`, in `BG` on the dark bezel. Use
   `"DOT MATRIX WITH STEREO SOUND"` **only if** the Bluetooth plan's audio task
   has landed and audio is real; otherwise use `"DOT MATRIX ON ELECTRONIC PAPER"`.
   Decide at implementation time and record the choice in the commit message.
3. **Labels.** `A` and `B` centred below their discs, `START` and `SELECT`
   centred below their pills, `MENU` centred **inside** the MENU box — exactly
   where a real DMG puts them.
4. **Speaker grille**: six parallel diagonal `DARK` lines in the lower-right
   case area, drawn with the existing `hline`/`vline` primitives stepped
   diagonally.
5. **Wordmark** `koboy` lower-left, in `DARK`, where Nintendo's logotype sits —
   and **only** `koboy`.

Every one of these must be expressed in permille of the panel, like everything
else in this file, so one implementation still fits every panel size.

Then:

```c
void chrome_render_battery(uint8_t *fb, int stride, const koboy_profile *p,
                           const koboy_layout *l, int percent)
{
    (void)l;
    const int W = p->panel_w, H = p->panel_h;
    /* Left of the screen, like the DMG's power LED. */
    int cx = p->game_x / 2;
    int cy = p->game_y + p->game_h / 2;
    int r  = W / 60;
    if (r < 4) r = 4;

    /* Never inside the game rect: the contract chrome_render lives under, and
       this function is called from the same places. */
    if (cx + r >= p->game_x) return;

    disc(fb, stride, W, H, cx, cy, r, BG);
    ring(fb, stride, W, H, cx, cy, r, INK);
    if (percent >= 0) {
        /* Fill proportionally, so the lamp says something rather than merely
           existing. */
        int fill = r * 2 * percent / 100;
        for (int y = cy + r - fill; y <= cy + r; y++)
            hline(fb, stride, W, H, cx - r, cx + r, y, DARK);
        ring(fb, stride, W, H, cx, cy, r, INK);
    }
    text_draw_centred_at(fb, stride, W, H, cx, cy + r + r / 2, "BATTERY",
                         1, INK);
}
```

Add the two small helpers this needs beside the existing `disc`/`box`:
`ring()` (a `disc` outline) and `text_draw_centred_at()` (centre a string on a
given x rather than on the panel) — or inline the arithmetic with
`text_measure`, whichever reads better in this file.

- [ ] **Step 5: Call it from `main.c`**

In `redraw_chrome`, after `chrome_render(...)` and before the blit:

```c
    /* Free, because the panel is already being repainted. */
    chrome_render_battery(panel, stride, prof, layout,
                          pf->battery_percent ? pf->battery_percent(pf->ctx) : -1);
```

Do the same at the initial chrome draw in `main()`.

- [ ] **Step 6: Regenerate and REVIEW the goldens**

```bash
rm -f tests/golden/chrome_*.pgm
KOBOY_GOLDEN_UPDATE=1 make build/test_chrome && KOBOY_GOLDEN_UPDATE=1 ./build/test_chrome
```

Open `tests/golden/chrome_1264x1680.pgm` in an image viewer and confirm it looks
like a Game Boy: asymmetric bezel, labelled buttons, battery lamp left of the
screen, grille lower-right, `koboy` lower-left. **Do not accept a golden you
have not looked at** — that is the whole reason `pgm_compare_golden` refuses to
auto-update an existing one.

- [ ] **Step 7: Run the tests**

Run: `make test`
Expected: PASS.

- [ ] **Step 8: Verify the game-rect contract is real (mutant)**

In `chrome_render_battery`, delete `if (cx + r >= p->game_x) return;` and force
`cx = p->game_x + 10`.
Expected: the `intruded == 0` check FAILS. Revert. Record the output.

- [ ] **Step 9: Verify the trademark check is real (mutant)**

Add a comment containing the word `Nintendo` to `src/chrome.c`.
Expected: the trademark check FAILS. Remove it. Record the output.

- [ ] **Step 10: Commit**

```bash
git add src/platform_if.h src/platform_sdl.c src/platform_kobo.c \
        src/chrome.h src/chrome.c src/main.c tests/test_chrome.c tests/golden
git commit -m "feat: a DMG-faithful faceplate with labels and a battery lamp

A, B, Start and Select were four indistinguishable grey shapes. They now
carry labels, below the buttons exactly where the real thing puts them,
which is what text.c was extracted for.

The chrome is drawn with GC16 -- sixteen levels -- and used three of
them. The four-level ceiling constrains the game rect only, so a tonal
ramp is free: chrome is drawn once, which makes elaborateness an
authoring question rather than a performance one.

The battery lamp is live but costs nothing: it is redrawn only when the
whole panel is already being repainted. A dedicated timer was rejected --
on a panel where every refresh is visible, adding a periodic one to show a
number that changes over hours is a bad trade.

No Nintendo word marks: homage to the industrial design, none of the
marks, asserted by a test on the source because that is where a future
edit would add one."
```

---

### Task 13: Multi-rect dirty regions

Closes follow-ups **#4** (`force_dither` never runs end-to-end), **#12**
(`g_bayer` threading note) and **#16** (`video_scale_gray` preconditions).

**Files:**
- Modify: `src/video.h`, `src/video.c`
- Modify: `src/config.h`, `src/config.c` (`refresh_fixed_tiles`)
- Modify: `src/main.c` (loop over the returned rects)
- Create: `tests/test_video_multirect.c`
- Modify: `tests/test_video_pipeline.c` (#4)
- Modify: `config/koboy.ini`

**Interfaces:**
- Consumes: `koboy_stats` (Task 1) for the tuning run.
- Produces:
  - `#define KOBOY_MAX_RECTS 4`
  - `int video_submit_rects(koboy_video *v, const void *src, int w, int h, size_t pitch, koboy_pixfmt fmt, koboy_rect *out, int max_out)` — returns the count
  - `int video_split_dirty(const uint8_t *prev, const uint8_t *cur, int w, int h, int stride, int fixed_tiles, koboy_rect *out, int max_out)`
  - `koboy_config.refresh_fixed_tiles`

- [ ] **Step 1: Write the failing test**

Create `tests/test_video_multirect.c`:

```c
#include "test.h"
#include "video.h"
#include <string.h>

/* Marks an 8x8-aligned block as changed. */
static void dirty_block(uint8_t *cur, int stride, int bx, int by, int bw, int bh)
{
    for (int y = by * KOBOY_TILE; y < (by + bh) * KOBOY_TILE; y++)
        for (int x = bx * KOBOY_TILE; x < (bx + bw) * KOBOY_TILE; x++)
            cur[(size_t)y * stride + x] = 0x00;
}

/* Every changed tile must be covered by SOME emitted rect. A split that drops
   a region leaves a stale pixel on the panel, which is indistinguishable from
   ghosting and therefore the kind of bug nobody reports. */
static int covers_all_dirty(const uint8_t *prev, const uint8_t *cur,
                            int w, int h, int stride,
                            const koboy_rect *r, int n)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t off = (size_t)y * stride + (size_t)x;
            if (prev[off] == cur[off]) continue;
            int covered = 0;
            for (int i = 0; i < n && !covered; i++)
                if (x >= r[i].x && x < r[i].x + r[i].w &&
                    y >= r[i].y && y < r[i].y + r[i].h) covered = 1;
            if (!covered) return 0;
        }
    }
    return 1;
}

static long total_area(const koboy_rect *r, int n)
{
    long a = 0;
    for (int i = 0; i < n; i++) a += (long)r[i].w * r[i].h;
    return a;
}

TEST_MAIN({
    enum { W = 320, H = 288, S = 320 };
    static uint8_t prev[W * H], cur[W * H];
    koboy_rect out[KOBOY_MAX_RECTS];

    /* Nothing changed: zero rects, and the caller refreshes nothing. */
    memset(prev, 0xFF, sizeof prev);
    memset(cur, 0xFF, sizeof cur);
    CHECK_EQ_INT(video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS), 0);

    /* One small change: one rect, and it must be small. Splitting a single
       compact region would only add fixed cost. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 1, 1, 2, 2);
    int n = video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS);
    CHECK_EQ_INT(n, 1);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));
    CHECK(total_area(out, n) < (long)W * H / 4);

    /* THE WIN CASE: a sprite top-left and a status bar bottom-right. Merged,
       the bounding box is nearly the whole rect and its interior has not
       changed. Split, the two pieces are tiny. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 0, 0, 2, 2);
    dirty_block(cur, S, W / KOBOY_TILE - 2, H / KOBOY_TILE - 2, 2, 2);
    n = video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS);
    CHECK(n >= 2);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));
    CHECK(total_area(out, n) < (long)W * H / 2);

    /* THE SCROLLER CASE: everything changed. One rect is correct -- splitting
       would pay N times the fixed cost for the same area. This is the case the
       spec is honest about: no rectangle strategy helps a full-screen scroller. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 0, 0, W / KOBOY_TILE, H / KOBOY_TILE);
    n = video_split_dirty(prev, cur, W, H, S, 40, out, KOBOY_MAX_RECTS);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(out[0].w, W);
    CHECK_EQ_INT(out[0].h, H);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));

    /* A very large fixed cost must suppress splitting entirely: at that point
       one big refresh genuinely is cheaper than several small ones. This is
       why the constant is configurable rather than compiled in. */
    memcpy(cur, prev, sizeof cur);
    dirty_block(cur, S, 0, 0, 2, 2);
    dirty_block(cur, S, W / KOBOY_TILE - 2, H / KOBOY_TILE - 2, 2, 2);
    n = video_split_dirty(prev, cur, W, H, S, 1000000, out, KOBOY_MAX_RECTS);
    CHECK_EQ_INT(n, 1);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));

    /* max_out is honoured, and coverage still holds when the split is capped. */
    memcpy(cur, prev, sizeof cur);
    for (int i = 0; i < 8; i++) dirty_block(cur, S, i * 4, i * 3, 1, 1);
    n = video_split_dirty(prev, cur, W, H, S, 40, out, 2);
    CHECK(n >= 1 && n <= 2);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));

    /* max_out of 1 always degrades to the merged bounding box. */
    n = video_split_dirty(prev, cur, W, H, S, 40, out, 1);
    CHECK_EQ_INT(n, 1);
    CHECK(covers_all_dirty(prev, cur, W, H, S, out, n));
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_video_multirect`
Expected: FAIL — `implicit declaration of function 'video_split_dirty'`

- [ ] **Step 3: Implement the split**

In `src/video.h`:

```c
#define KOBOY_MAX_RECTS 4

/* Splits the changed region into up to max_out disjoint rectangles, or returns
   the single merged bounding box when splitting would not pay.

   Refresh cost on this hardware is roughly `fixed + area`, so a sprite in the
   top-left and a status bar in the bottom-right merge into a near-full-rect
   refresh whose interior has not changed. `fixed_tiles` expresses the fixed
   cost in 8x8 tiles and comes from config, not from a constant: every absolute
   timing this project has measured moved by up to a factor of 2.2 between
   sessions, so a compiled-in threshold would be false precision.

   Returns 0 when nothing changed. The union of the returned rects ALWAYS covers
   every changed tile -- a dropped region leaves a stale pixel that looks exactly
   like ghosting, which is the worst kind of e-ink bug because nobody reports it
   as a bug. tests/test_video_multirect.c asserts that union directly. */
int video_split_dirty(const uint8_t *prev, const uint8_t *cur,
                      int w, int h, int stride, int fixed_tiles,
                      koboy_rect *out, int max_out);

/* As video_submit, but fills `out` with up to max_out rects and returns the
   count. video_submit remains as a single-rect wrapper for existing callers
   and tests. */
int video_submit_rects(koboy_video *v, const void *src, int src_w, int src_h,
                       size_t src_pitch, koboy_pixfmt fmt,
                       koboy_rect *out, int max_out);
```

In `src/video.c`, implement:

- A tile-dirty bitmap pass, reusing `video_dirty_rect`'s existing `memcmp`
  walk but recording each changed tile.
- **Row-band segmentation:** maximal runs of consecutive dirty tile-rows
  separated by at least one clean tile-row. Compute each band's bounding box.
- **Column segmentation within each band**, by the same rule, while the total
  rect count stays under `max_out`.
- **The decision:** with `cost(r) = fixed_tiles + tiles_in(r)`, emit the split
  only when `Σ cost(sub) < cost(merged)`. Otherwise emit the merged box.
- Guard `max_out < 1` by returning the merged box in a caller-provided slot;
  guard `out == NULL` by returning 0.

`video_submit_rects` runs the existing convert/scale/quantise/dither pipeline
unchanged, then calls `video_split_dirty` instead of `video_dirty_rect`, and
copies `cur` to `prev` when any rect was emitted.

Keep `video_submit` as:

```c
koboy_rect video_submit(koboy_video *v, const void *src, int src_w, int src_h,
                        size_t src_pitch, koboy_pixfmt fmt)
{
    koboy_rect one = { 0, 0, 0, 0 };
    koboy_rect r[1];
    int n = video_submit_rects(v, src, src_w, src_h, src_pitch, fmt, r, 1);
    if (n > 0) one = r[0];
    return one;
}
```

- [ ] **Step 4: Add `refresh_fixed_tiles` to the config**

`src/config.h`: `int refresh_fixed_tiles;`
`config_defaults`: `c->refresh_fixed_tiles = 40;`
`config_load`: `else if (!strcmp(k, "refresh_fixed_tiles")) c->refresh_fixed_tiles = atoi(v);`

In `config/koboy.ini`:

```
# The fixed per-refresh cost, expressed in 8x8 tiles, used to decide whether
# splitting one dirty rectangle into several is cheaper than refreshing their
# merged bounding box. Refresh cost on e-ink is roughly `fixed + area`, so a
# sprite in one corner and a status bar in another merge into a near-full-rect
# refresh whose interior never changed.
# This is a config key rather than a constant because every absolute timing
# figure this project has measured moved by up to a factor of 2.2 between
# sessions. Raise it to split less; a very large value never splits.
# NOTE: this does nothing for a full-screen scroller. That case is the panel's
# answer, not the algorithm's, and no rectangle strategy improves it.
refresh_fixed_tiles = 40
```

- [ ] **Step 5: Use the rects in the emulator loop**

In `src/main.c`, replace the single-rect blit/refresh with a loop:

```c
        koboy_rect rects[KOBOY_MAX_RECTS];
        t0 = pf->now_us(pf->ctx);
        int nrects = video_submit_rects(vid, g_frame, (int)g_fw, (int)g_fh,
                                        g_fpitch, core_pixfmt(core),
                                        rects, KOBOY_MAX_RECTS);
        stats_add(&stats, KOBOY_STAGE_SUBMIT, pf->now_us(pf->ctx) - t0);
        if (nrects == 0) goto sram_check;

        long dirty_px = 0;
        for (int i = 0; i < nrects; i++) dirty_px += (long)rects[i].w * rects[i].h;

        koboy_refresh_mode mode = KOBOY_REFRESH_FAST;
        if (config_promote_full(&cfg, dirty_px,
                                (long)prof.game_w * (long)prof.game_h)) {
            mode = KOBOY_REFRESH_FULL;
            big_refreshes++;
        }

        for (int i = 0; i < nrects; i++) {
            const koboy_rect *r = &rects[i];
            t0 = pf->now_us(pf->ctx);
            pf->blit_gray8(pf->ctx,
                           video_buffer(vid) + (size_t)r->y * video_stride(vid) + r->x,
                           r->w, r->h, video_stride(vid),
                           prof.game_x + r->x, prof.game_y + r->y);
            stats_add(&stats, KOBOY_STAGE_BLIT, pf->now_us(pf->ctx) - t0);

            t0 = pf->now_us(pf->ctx);
            pf->refresh(pf->ctx, prof.game_x + r->x, prof.game_y + r->y,
                        r->w, r->h, mode);
            stats_add(&stats, KOBOY_STAGE_REFRESH, pf->now_us(pf->ctx) - t0);
        }
        presented++;
        rects_emitted += (unsigned long)nrects;
```

Declare `unsigned long rects_emitted = 0;` beside `presented`, and add it to the
exit summary: `%lu rects over %lu presented frames`. Keep the existing waveform
comment block, which explains why the promotion exists — it now applies to the
**total** dirty area rather than one rect, and the comment should say so.

- [ ] **Step 6: Close #4, #12 and #16**

**#4** — in `tests/test_video_pipeline.c`, add an end-to-end run with
`force_dither = true` asserting the output contains only 0x00 and 0xFF (the
1-bit dither's two values) while the un-dithered run contains the four
`KOBOY_DU4_LEVELS`. The dither component is well tested directly; the
`if (v->dither)` wiring a GBC user hits is not.

**#12** — in `src/video.c`, above `bayer_ensure`:

```c
/* Lazy init, and single-threaded by construction: nothing in src/ creates a
   thread, and §6 of the design keeps the emulator single-threaded because
   non-blocking refresh submission removed the reason to add one. If a worker
   thread ever appears, this needs a once-guard. */
```

**#16** — in `src/video.c`, above `video_scale_gray`:

```c
/* Preconditions, satisfied by construction at the only call site and stated
   because nothing enforces them: scale >= 1, and dst_stride >= src_w * scale.
   A scale of 0 emits nothing; a short dst_stride overlaps rows. */
```

- [ ] **Step 7: Run everything**

Run: `make test && bash tests/smoke_host.sh`
Expected: PASS, including the existing `test_video_dirty` (unchanged behaviour
through the `video_submit` wrapper).

- [ ] **Step 8: Verify the coverage property is real (mutant)**

In `video_split_dirty`'s band segmentation, drop the last band before returning
(`if (n > 1) n--;`).
Expected: `covers_all_dirty` FAILS on the two-corner case. This is the mutant
that matters most in this task: without it a dropped region is invisible in
every other test and shows up on the panel as ghosting. Revert. Record the
output.

- [ ] **Step 9: Verify the cost model is real (mutant)**

Force the split to always be taken (delete the `Σ cost(sub) < cost(merged)`
comparison).
Expected: the full-screen-scroller case FAILS with `n > 1` — the algorithm now
pays N times the fixed cost for the same area. Revert. Record the output.

- [ ] **Step 10: ON DEVICE — tune `refresh_fixed_tiles`**

Deploy and run Zelda for a few minutes. From `koboy.log` record the `stages`
line and the `rects` count, at `refresh_fixed_tiles` of 20, 40 and 80.

Pick the value with the lowest mean `refresh` cost per presented frame. If none
of them beats a value large enough to disable splitting, **say so and ship the
splitting disabled** — the honest outcome is a measurement, not a feature.
Record the numbers in `TESTED.md`.

- [ ] **Step 11: Commit**

```bash
git add src/video.h src/video.c src/config.h src/config.c src/main.c \
        config/koboy.ini tests/test_video_multirect.c tests/test_video_pipeline.c
git commit -m "feat: multi-rect dirty regions, decided by a measured cost model

A sprite top-left and a status bar bottom-right merged into a near-full
refresh whose interior never changed. Up to four disjoint rects are
emitted instead, but only when the summed cost beats the merged box.

The fixed cost is a config key, not a constant: every absolute timing
this project has measured moved by up to a factor of 2.2 between
sessions, so a compiled-in threshold would be false precision.

This does NOTHING for a full-screen scroller, and the code says so. That
case is the panel's answer, not the algorithm's.

It is a correctness change as much as a speed one: the union of emitted
rects must cover every dirty tile, or the panel keeps a stale pixel that
looks exactly like ghosting. Mutant: dropping the last band makes the
coverage assertion fail while every other test still passes.

Closes #4 (force_dither now runs end to end), #12 and #16."
```

---

### Task 14: Packaging, docs and the device matrix

Closes follow-ups **#10** (`verify-core.sh`'s allowlist is unanchored) and
**#15** (`docs/probe-readme.md` never names `make probe-dist`).

**Files:**
- Modify: `scripts/verify-core.sh`
- Modify: `docs/probe-readme.md`
- Modify: `tests/test_dist.sh`
- Modify: `README.md`, `TESTED.md`
- Modify: `Makefile` (ship the `roms/` directory)
- Modify: `CLAUDE.md`

**Interfaces:**
- Consumes: everything above.
- Produces: a `dist` zip whose closure still verifies.

- [ ] **Step 1: Write the failing assertion for the anchored allowlist (#10)**

Append to `tests/test_dist.sh`, in its existing assertion style:

```sh
# #10: the allowlist matched by substring, so a library named reallibc.so.6
# would have passed. Not attacker-facing -- it is a build-time script -- but
# anchors are free, and an allowlist that accepts a superstring is not an
# allowlist.
fake="$(mktemp -d)"
cat > "$fake/readelf" <<'EOS'
#!/bin/sh
echo " 0x00000001 (NEEDED)  Shared library: [reallibc.so.6]"
EOS
chmod +x "$fake/readelf"
if PATH="$fake:$PATH" sh scripts/verify-core.sh /bin/true >/dev/null 2>&1; then
    echo "FAIL: verify-core.sh accepted reallibc.so.6"
    rm -rf "$fake"; exit 1
fi
rm -rf "$fake"
echo "ok: verify-core.sh allowlist is anchored"
```

Adjust the stub to match however `verify-core.sh` actually invokes its reader
(`readelf`, `eu-readelf` or `lddtree`) and however it takes its argument — read
the script first and match it exactly rather than guessing.

- [ ] **Step 2: Run it to verify it fails**

Run: `bash tests/test_dist.sh`
Expected: FAIL — `verify-core.sh accepted reallibc.so.6`.

- [ ] **Step 3: Anchor the allowlist**

In `scripts/verify-core.sh` line 31, the allowlist test matches by substring.
Replace it with an exact, whole-token comparison — a `case` on the exact library
name against each permitted value:

```sh
    case "$lib" in
        libm.so.6|libc.so.6|ld-linux-armhf.so.3) ;;
        *) echo "verify-core: unexpected dependency: $lib"; rc=1 ;;
    esac
```

Keep the existing message text and exit-code behaviour so nothing else that
parses this script's output breaks.

- [ ] **Step 4: Run it to verify it passes**

Run: `bash tests/test_dist.sh`
Expected: PASS, including the new assertion.

- [ ] **Step 5: Verify the anchor is real (mutant)**

Revert `verify-core.sh` to the substring test.
Expected: the new assertion FAILS. Revert the mutant. Record the output.

- [ ] **Step 6: Document `make probe-dist` (#15)**

In `docs/probe-readme.md`, add a build section naming the lightweight target.
Today the document never mentions it, so a contributor follows the heavier
`make dist` — which needs a cross-built gambatte core they do not want:

```markdown
## Building just the probe

    export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
    make probe-dist        # -> dist/koboy-probe-0.1.0.zip

This builds the probe and nothing else. `make dist` also cross-builds gambatte,
which takes minutes and which you do not need in order to characterise a device.
```

- [ ] **Step 7: Ship a `roms/` directory**

The browser needs somewhere to look, and an empty directory in a zip is easy to
get wrong. In the `dist` target, after the other `cp` lines:

```make
	mkdir -p build/pkg/.adds/koboy/roms
	printf 'Put .gb and .gbc files in this directory.\nkoboy lists them at startup.\n' \
	    > build/pkg/.adds/koboy/roms/README.txt
```

The `README.txt` is not decoration: `zip -qrD` omits directory entries, so an
empty `roms/` would not appear in the archive at all and the browser's first run
would report a missing directory.

Add an assertion to `tests/test_dist.sh` that `.adds/koboy/roms/README.txt` is
present in the zip listing.

- [ ] **Step 8: Update the docs**

`README.md`: document the browser, the menu, save states, `rom_dir` and
`refresh_fixed_tiles`.

`TESTED.md`: add to the existing Libra 2 row, or as a new sub-section, the
results from Task 1 Step 11 (the Zelda SRAM verification — the first time the
save path ran on hardware) and Task 13 Step 10 (the `refresh_fixed_tiles`
tuning). State plainly what is still unverified.

`CLAUDE.md`: update **Known unfinished**. The line *"The save path has never run
on hardware"* is retired by Task 1 Step 11; the line about `dpad_mode = cross`
having no test is retired by Task 8. Do not delete them silently — replace them
with what is true now, and add the new v2 surface (the UI modes, save states,
`--ui-script`) to the layout table.

- [ ] **Step 9: Full verification, host and device**

```bash
make test
bash tests/smoke_host.sh
bash tests/test_dist.sh
export PATH="$HOME/.cache/koboy-toolchain/arm-linaro-4.9-2014.09/bin:$PATH"
make kobo
make dist
bash scripts/verify-core.sh build/koboy-arm
```

Expected: all pass, and the closure is exactly `libm.so.6`, `libc.so.6`,
`ld-linux-armhf.so.3`. **This is the constraint every new module in this plan
was written to preserve** — `text.c`, `ui.c`, `romlist.c`, `uiscript.c`,
`state.c`, `safefile.c` and `stats.c` add nothing beyond libc.

If `verify-core.sh` reports anything else, stop: a new dependency crept in and
must be removed, not allowlisted.

- [ ] **Step 10: Commit**

```bash
git add scripts/verify-core.sh docs/probe-readme.md tests/test_dist.sh \
        Makefile README.md TESTED.md CLAUDE.md
git commit -m "chore: packaging, docs and the v2 device matrix

Closes #10: the dependency allowlist matched by substring, so a library
named reallibc.so.6 would have passed. An allowlist that accepts a
superstring is not an allowlist. Closes #15: probe-readme never named
make probe-dist, so a contributor ran the heavier full dist and
cross-built gambatte for nothing.

Ships .adds/koboy/roms/ with a README.txt, because zip -qrD omits
directory entries and an empty roms/ would simply not be in the archive.

TESTED.md records the two device runs this plan produced: the Zelda SRAM
verification -- the first time the save path has ever executed on
hardware -- and the refresh_fixed_tiles tuning.

CLAUDE.md's Known unfinished loses two entries that are now false."
```

---

## Plan self-review

**Spec coverage.** Every section of
`docs/superpowers/specs/2026-08-25-koboy-v2-design.md` maps to a task:

| Spec | Task |
|---|---|
| §2 modules, mode machine, `video_invalidate`, `redraw_chrome` | 2, 3, 4, 5, 6 |
| §3 list widget, edge triggering, page-turn paging | 3 |
| §4 MENU zone, `chrome_controls_top`, menu structure, why not power | 8, 11 |
| §5 save states, optional binds, storage, three lifecycle hazards, SRAM gap | 1 (step 11), 9, 10, 11 |
| §6 faceplate, tonal ramp, trademark, live battery lamp, invariants | 12 |
| §7 measurement first, the split, honest limit, coverage correctness | 1, 13 |
| §8 Bluetooth | **companion plan** — `2026-08-25-koboy-v2-bluetooth.md` |
| §9 follow-up closure (14 of 16 here; #3 in task 1) | 6, 7, 8, 9, 13, 14 |
| §10 testing, the scripted-path trap, mutants | 5, 6, and every task's mutant step |
| §11 build order | task order |
| §12 risks | 8 (`controls_top`), 10 (all-or-nothing), 13 (coverage) |
| §13 open measurements | 1 (step 11), 13 (step 10) |

Follow-ups: #1 → task 8, #2 → task 8, #3 → task 1, #4 → task 13, #5 → task 6,
#6 → task 7, #7 → task 9, #8 → task 7, #9 → task 7, #10 → task 14, #11 → task 9,
#12 → task 13, #13 → task 7, #14 → task 6, #15 → task 14, #16 → task 13. **All
sixteen are covered.**

**Type consistency.** `koboy_rect`, `koboy_input_state`, `koboy_profile` and
`koboy_layout` are used as `src/koboy.h` defines them. `ui_list_feed` has the
same signature everywhere it appears (tasks 3, 6, 11). `video_submit_rects`
(task 13) and `video_submit` (task 6) coexist deliberately, with the latter a
wrapper, so task 6's `video_invalidate` test keeps working unchanged.
`state_path`'s 1-based slot convention is consistent between task 10 and task 11.
`safefile_write`/`safefile_read_exact` are named identically in tasks 10 and 11.

**Known gap, stated rather than hidden.** `--ui-script` drives `run_list` in
`MODE_BROWSE` (task 6) but the emulator loop does not accept scripted input, so
`MODE_MENU` is exercised interactively and by construction, not by an automated
run. Task 11 Step 5 says so in the smoke test's own comment rather than
implying coverage that does not exist. Extending `--ui-script` through the
emulator loop is the obvious follow-up and belongs in v2's own FOLLOWUPS entry.
