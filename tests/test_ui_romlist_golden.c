#include "test.h"
#include "pgm.h"
#include "ui.h"
#include <string.h>

/* Not a functional check -- pgm_compare_golden already covers pixel
   regression the same way every other golden test in this suite does. This
   file exists so "fit substantially more rows, and verify by looking at the
   render" (the ROM browser density work) has an artifact to look AT: one
   full page of realistic titles at the shipped 1264x1680 geometry, with the
   letter-index strip on, the way the goldens under tests/golden already do
   for the chrome faceplate. Convert tests/golden/romlist_dense.pgm to a PNG
   to review it -- `pnmtopng` or ImageMagick's `convert` both read PGM. */
TEST_MAIN({
    /* One full page (rows == 24 at this geometry -- see src/ui.c) of names a
       real collection would produce: two names from the actual bug report
       (Shantae, and the same title again under gbc/ to show the
       subdirectory disambiguation prefix), the longest real title this
       project has on record, and a spread of starting letters -- including
       gaps -- so the strip's "present in full ink, absent in dim ink"
       distinction is visible rather than every band looking the same. */
    static const char *const items[] = {
        "0 Golf (Japan).gb",
        "Alone in the Dark - The New Nightmare (Europe).gbc",
        "Bomberman GB (USA).gb",
        "Bomberman Max - Blue Champion (USA).gbc",
        "Castlevania Legends (USA).gb",
        "Donkey Kong Land (USA, Europe).gb",
        "Dragon Warrior Monsters (USA) (SGB Enhanced).gb",
        "F-1 Race (World).gb",
        "Kirby's Dream Land 2 (USA, Europe).gb",
        "Legend of Zelda, The - Link's Awakening (USA, Europe) (Rev 2).gb",
        "Metroid II - Return of Samus (World).gb",
        "Mole Mania (USA).gb",
        "Pokemon Crystal Version (USA, Europe).gbc",
        "Pokemon Yellow Version - Special Pikachu Edition (USA, Europe) (CGB+SGB Enhanced).gb",
        "Q-Billion (Europe).gb",
        "R-Type (World).gb",
        "Shantae (USA).gbc",
        "gbc/Shantae (USA).gbc",
        "Super Mario Land (World).gb",
        "Tetris (World) (Rev 1).gb",
        "Trip World (Europe).gb",
        "Wario Land - Super Mario Land 3 (USA, Europe).gb",
        "Wave Race (USA, Europe).gb",
        "Yoshi (USA, Europe).gb",
    };
    const int N = (int)(sizeof items / sizeof items[0]);

    enum { W = 1264, H = 1680, MARGIN = 8 };
    koboy_ui_list list;
    ui_list_init(&list, "CHOOSE A GAME", items, N,
                 MARGIN, MARGIN, W - 2 * MARGIN, H - 2 * MARGIN);
    ui_list_enable_alpha_jump(&list, true);

    /* This geometry claims 24 rows a page -- pin it down here too, the same
       way tests/test_ui.c does, so a regression in UI_MAX_ROWS or the row_h
       derivation fails an assertion instead of just quietly re-drawing a
       different (and unreviewed) golden. */
    CHECK_EQ_INT(ui_list_rows(&list), 24);
    CHECK_EQ_INT(N, 24);            /* this page is exactly full, on purpose */

    static uint8_t fb[W * H];
    memset(fb, 0xFF, sizeof fb);
    ui_list_render(&list, fb, W, W, H);

    CHECK(pgm_compare_golden("romlist_dense", fb, W, H, W));
})
