#include "stats.h"
#include <stdio.h>
#include <string.h>

/* Live bounds check, not dead code: main.c passes a stage index derived from
   control flow, and an out-of-range write here would silently corrupt the
   neighbouring stage's totals -- producing a plausible-looking wrong number in
   a bug report, which is worse than a crash. Exposed (see stats.h) rather than
   static, so the test suite can assert it directly instead of trying to
   observe it through stats_add: an out-of-range write from a broken guard here
   lands outside the struct or aliases a different field, so probing it via
   stats_add would detect a broken guard only by happening to crash on a given
   compiler/layout -- the same UB-dependent-test mistake chrome_bands in
   src/chrome.c exists to avoid. */
bool stats_stage_valid(int stage) { return stage >= 0 && stage < KOBOY_STAGE_COUNT; }

void stats_reset(koboy_stats *s) { memset(s, 0, sizeof *s); }

void stats_add(koboy_stats *s, int stage, uint64_t us)
{
    if (!stats_stage_valid(stage)) return;
    s->total_us[stage] += us;
    s->count[stage]++;
    if (us > s->max_us[stage]) s->max_us[stage] = us;
}

uint64_t stats_mean_us(const koboy_stats *s, int stage)
{
    if (!stats_stage_valid(stage) || s->count[stage] == 0) return 0;
    return s->total_us[stage] / s->count[stage];
}

uint64_t stats_max_us(const koboy_stats *s, int stage)
{
    return stats_stage_valid(stage) ? s->max_us[stage] : 0;
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
