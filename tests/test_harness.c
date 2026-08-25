#include "test.h"
#include "pgm.h"

TEST_MAIN({
    uint8_t img[4 * 4];
    for (int i = 0; i < 16; i++) img[i] = (uint8_t)(i * 16);
    CHECK(pgm_compare_golden("harness_ramp", img, 4, 4, 4) == 1);
    CHECK_EQ_INT(2 + 2, 4);
})
