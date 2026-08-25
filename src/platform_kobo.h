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
