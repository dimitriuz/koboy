#ifndef KOBOY_PACING_H
#define KOBOY_PACING_H
#include "koboy.h"

typedef struct { uint64_t start_us, frames; int divisor; } koboy_pacer;

void     pacer_init(koboy_pacer *p, uint64_t now_us, int divisor);
uint64_t pacer_delay_us(const koboy_pacer *p, uint64_t now_us);
bool     pacer_tick(koboy_pacer *p);
#endif
