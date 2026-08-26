#include "btinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* BUS_BLUETOOTH, from <linux/input.h> -- mirrored rather than included, the
   same rule koboy.h and input.h follow so portable code never pulls that
   header in. */
#define KOBOY_BUS_BLUETOOTH 0x0005ul

/* BTN_GAMEPAD's range: BTN_SOUTH(0x130) through BTN_THUMBR(0x13F), the eight
   codes any gamepad-shaped device advertises at least one of. 0x130-0x13F is
   exactly the top 16 bits (48-63) of the 64-bit KEY= word that covers codes
   256-319 -- see the indexing comment in has_gamepad_key() below. */
#define BTN_GAMEPAD_LO 0x130u

/* A record is capped at this many KEY= words (2048 bits) when hunting for a
   BTN_GAMEPAD-range bit. Real records top out around 12 words (KEY_MAX is
   0x2ff), so this is generous headroom, not a tight fit -- but it is a
   bound: this parses a kernel-emitted file whose exact width is not under
   koboy's control, and "however many words appear" is not a length a fixed
   array can hold without one. */
#define MAX_KEY_WORDS 32

/* Scans for a line starting with `prefix` and returns a pointer just past it,
   or NULL. Anchored at column 0 of each line rather than a bare strstr,
   because strstr would also match `prefix` appearing mid-line -- e.g. inside
   a quoted device Name -- and every I:/N:/H:/B: line in the real file starts
   at column 0. */
static const char *find_line_prefix(const char *record, const char *prefix)
{
    if (!record) return NULL;
    size_t plen = strlen(prefix);
    const char *p = record;
    while (p && *p) {
        if (!strncmp(p, prefix, plen)) return p + plen;
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* strtoul/strtoull stop at the first non-hex character, so this is safe even
   against a truncated or malformed field -- live guard: this reads a kernel
   file, and "the format never changes" is not a promise the kernel makes. */
static unsigned long parse_hex(const char *s)
{
    return s ? strtoul(s, NULL, 16) : 0;
}

/* True if the `B: KEY=` line in `record` has any bit set in the BTN_GAMEPAD
   range (0x130-0x13F / 304-319 decimal).

   The line is a space-separated run of 64-bit hex words, MOST SIGNIFICANT
   WORD FIRST (documented kernel behaviour, and matches every captured
   record this project has). Codes 256-319 live in the word that is 4 words
   in from the LEAST significant end (256 / 64 == 4) -- and because the words
   are printed most-significant-first, which word that is from the START of
   the line depends on how many words the line has, which varies by device.
   So the word count is measured first, not assumed. Once the right word is
   found, 0x130-0x13F is exactly its top 16 bits (bits 48-63, since
   256 + 48 == 304 and 256 + 63 == 319), so a single shift-and-mask answers
   the question -- no bit-by-bit walk needed. */
static bool has_gamepad_key(const char *record)
{
    const char *key_s = find_line_prefix(record, "B: KEY=");
    if (!key_s) return false;

    const char *starts[MAX_KEY_WORDS];
    size_t      lens[MAX_KEY_WORDS];
    int         words = 0;

    const char *p = key_s;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != '\n') {
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (words < MAX_KEY_WORDS) {
            starts[words] = tok;
            lens[words]   = (size_t)(p - tok);
            words++;
        }
        /* A record with more words than MAX_KEY_WORDS keeps counting past
           the array so `words` (used below only for indexing arithmetic,
           never to write) stays the true count -- but bytes past the cap are
           never stored, so this cannot overrun `starts`/`lens`. */
        else {
            words++;
        }
        while (*p == ' ' || *p == '\t') p++;
    }
    if (words == 0) return false;

    const int right_index = (int)(BTN_GAMEPAD_LO / 64);   /* = 4, see comment above */
    if (words <= right_index) return false;   /* too few words to reach that range */
    int left_index = words - 1 - right_index;
    if (left_index < 0 || left_index >= MAX_KEY_WORDS) return false; /* live: see the cap above */

    char buf[24];   /* a 64-bit hex word is at most 16 digits */
    size_t len = lens[left_index];
    if (len >= sizeof buf) len = sizeof buf - 1;   /* live: never overrun buf on a malformed field */
    memcpy(buf, starts[left_index], len);
    buf[len] = '\0';

    unsigned long long word = strtoull(buf, NULL, 16);
    return ((word >> 48) & 0xFFFFull) != 0;
}

bool btinput_is_gamepad(const char *record)
{
    if (!record || !*record) return false;

    const char *bus_s = find_line_prefix(record, "I: Bus=");
    if (!bus_s || parse_hex(bus_s) != KOBOY_BUS_BLUETOOTH) return false;

    const char *ev_s = find_line_prefix(record, "B: EV=");
    if (!ev_s) return false;
    unsigned long ev = parse_hex(ev_s);
    /* bit 1 = EV_KEY, bit 3 = EV_ABS. Both required: EV_KEY alone is a plain
       keyboard (or the gpio-keys page-turn node); EV_ABS alone is the
       accelerometer. Neither on its own is a gamepad. */
    if (!(ev & (1ul << 1)) || !(ev & (1ul << 3))) return false;

    /* The range check is what tells a gamepad apart from a Bluetooth
       keyboard or mouse that also happens to report Bus=0005 and both
       EV_KEY and EV_ABS -- some multimedia keyboards report ABS for a
       volume wheel, and BT mice report ABS for the scroll wheel, while
       neither advertises a single BTN_GAMEPAD-range key. Adopting either
       would push stray keys, or nothing useful at all, into the running
       game. */
    return has_gamepad_key(record);
}

int btinput_parse_handlers(const char *record, char *out, size_t n)
{
    if (!record || !out || !n) return 0;
    const char *h = find_line_prefix(record, "H: Handlers=");
    if (!h) return 0;

    const char *p = h;
    while (*p && *p != '\n') {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        size_t len = (size_t)(p - tok);
        if (len >= 5 && !strncmp(tok, "event", 5)) {
            /* Live bound: `n` is the caller's buffer, and this parses a
               kernel-emitted token whose width is not under koboy's
               control. Refuse rather than truncate -- a truncated node name
               would silently open the wrong device, or none. */
            if (len >= n) return 0;
            memcpy(out, tok, len);
            out[len] = '\0';
            return 1;
        }
    }
    return 0;
}

int btinput_scan(char *out_node, size_t n)
{
    if (!out_node || !n) return -1;

    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return -1;

    /* Bounded read. The real file lists a handful of devices and is a few
       KB; a cap here is a live guard against a kernel that someday emits
       more, not a limit expected to bite -- see MAX_KEY_WORDS above for the
       same reasoning applied to one line instead of the whole file. */
    static char buf[16384];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';

    /* Records are separated by one blank line; none of the I:/N:/H:/B: lines
       are ever themselves blank, so this split is unambiguous. */
    char *rec = buf;
    while (rec && *rec) {
        char *blank = strstr(rec, "\n\n");
        char *next  = blank ? blank + 2 : NULL;
        if (blank) *(blank + 1) = '\0';   /* keep the trailing \n, end the record there */

        if (btinput_is_gamepad(rec)) {
            char handler[64];
            if (btinput_parse_handlers(rec, handler, sizeof handler)) {
                int wrote = snprintf(out_node, n, "/dev/input/%s", handler);
                if (wrote > 0 && (size_t)wrote < n) return 1;
            }
        }
        rec = next;
    }
    return 0;
}
