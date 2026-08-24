#ifndef KOBOY_CHROME_H
#define KOBOY_CHROME_H
#include "koboy.h"

/* Draws the static faceplate into a full-panel gray8 buffer. Must never write
   inside the profile's game rect. Called once at startup and refreshed with
   KOBOY_REFRESH_FULL, so it costs nothing per frame. */
void chrome_render(uint8_t *fb, int stride, const koboy_profile *p,
                   const koboy_layout *l);
#endif
