#include "test.h"
#include "core.h"

static const void *last_data; static unsigned last_w, last_h;
static void on_frame(void *ud, const void *d, unsigned w, unsigned h, size_t pitch)
{
    (void)ud; (void)pitch; last_data = d; last_w = w; last_h = h;
}
static uint16_t g_buttons;
static uint16_t get_buttons(void *ud) { (void)ud; return g_buttons; }

TEST_MAIN({
    char err[256] = {0};

    /* a missing core reports an error rather than crashing, because on the
       device a silent failure is indistinguishable from a crash */
    CHECK(core_open("build/definitely-absent.so", ".", err, sizeof err) == NULL);
    CHECK(err[0] != 0);

    koboy_core *c = core_open("build/stub_core.so", "build", err, sizeof err);
    CHECK(c != NULL);
    CHECK_EQ_INT(core_pixfmt(c), KOBOY_PIXFMT_RGB565);

    core_set_frame_cb(c, on_frame, NULL);
    core_set_input_fn(c, get_buttons, NULL);

    FILE *f = fopen("build/fake.gb", "wb"); fputc(0, f); fclose(f);
    CHECK(core_load_rom(c, "build/fake.gb", err, sizeof err));

    size_t sl = 0;
    CHECK(core_sram(c, &sl) != NULL);
    CHECK_EQ_INT(sl, 8);

    /* the frame callback fires with Game Boy geometry */
    g_buttons = 0;
    core_run_frame(c);
    CHECK(last_data != NULL);
    CHECK_EQ_INT(last_w, 160);
    CHECK_EQ_INT(last_h, 144);

    /* buttons reach the core: the stub echoes the bitmask into pixel 0 */
    g_buttons = KOBOY_BTN_A | KOBOY_BTN_RIGHT;
    core_run_frame(c);
    CHECK_EQ_INT(((const uint16_t *)last_data)[0], KOBOY_BTN_A | KOBOY_BTN_RIGHT);

    core_close(c);
})
