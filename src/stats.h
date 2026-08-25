#ifndef KOBOY_STATS_H
#define KOBOY_STATS_H
#include <stdbool.h>
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

/* Exposed only so the bounds guard can be asserted DIRECTLY. Testing it by
   calling stats_add with an out-of-range index cannot work: those writes land
   outside the struct or alias a different field, so the test would detect a
   broken guard only by happening to crash. Same reasoning, and the same fix,
   as chrome_bands in src/chrome.c. */
bool     stats_stage_valid(int stage);

void     stats_reset(koboy_stats *s);
void     stats_add(koboy_stats *s, int stage, uint64_t us);
uint64_t stats_mean_us(const koboy_stats *s, int stage);
uint64_t stats_max_us(const koboy_stats *s, int stage);

/* Writes one line, always NUL-terminated, never exceeding n bytes. */
void     stats_format(const koboy_stats *s, char *out, size_t n);
#endif
