#include "test.h"
#include "btinput.h"
#include <string.h>

/* Real records, in the exact shape /proc/bus/input/devices emits. The three
   non-gamepad ones are copied from the verified Libra 2 (design spec
   Appendix A), so the filter is tested against the hardware it has to
   coexist with. */
static const char GPIO_KEYS[] =
    "I: Bus=0019 Vendor=0001 Product=0001 Version=0100\n"
    "N: Name=\"gpio-keys\"\n"
    "H: Handlers=event0 \n"
    "B: PROP=0\n"
    "B: EV=100013\n"
    "B: KEY=6 0 0 100000 0 8000000 0\n";

static const char TOUCH[] =
    "I: Bus=0018 Vendor=0000 Product=0000 Version=0000\n"
    "N: Name=\"Elan Touchscreen\"\n"
    "H: Handlers=kbd mouse0 event1 \n"
    "B: PROP=2\n"
    "B: EV=b\n"
    "B: ABS=ee18000 1000003\n";

static const char ACCEL[] =
    "I: Bus=0018 Vendor=001b Product=0000 Version=0000\n"
    "N: Name=\"kx122-accel\"\n"
    "H: Handlers=event2 \n"
    "B: PROP=0\n"
    "B: EV=9\n"
    "B: ABS=7\n";

/* A Bluetooth HID gamepad: Bus=0005 is BUS_BLUETOOTH, and it advertises both
   EV_KEY and EV_ABS with BTN_GAMEPAD-range keys. */
static const char PAD[] =
    "I: Bus=0005 Vendor=057e Product=2009 Version=0001\n"
    "N: Name=\"Pro Controller\"\n"
    "H: Handlers=event3 js0 \n"
    "B: PROP=0\n"
    "B: EV=20000b\n"
    "B: KEY=7fdb000000000000 0 0 0 0\n"
    "B: ABS=3f\n";

/* A Bluetooth keyboard is NOT a gamepad: it has EV_KEY but no gamepad buttons
   and no absolute axes. Adopting it would send stray keys into the game. */
static const char BT_KBD[] =
    "I: Bus=0005 Vendor=05ac Product=0255 Version=0011\n"
    "N: Name=\"Magic Keyboard\"\n"
    "H: Handlers=sysrq kbd event4 \n"
    "B: PROP=0\n"
    "B: EV=120013\n"
    "B: KEY=e0ff0f 0 0 0 0\n";

/* A REAL Xbox Wireless Controller, paired over Bluetooth to the verified
   Libra 2 and decoded from raw input_event records, 2026-08-26. Bus, Vendor,
   Product, Name, Handlers and EV are the measured values verbatim. The KEY=
   bitmap is reconstructed (the raw /proc dump was not captured, only the
   decoded button table) with bits set for exactly the four buttons the table
   names as EV_KEY: BTN_SOUTH(304)=A, BTN_EAST(305)=B, BTN_TL(310)=LB,
   BTN_TR(311)=RB. Those four codes are local bits 48,49,54,55 of the 64-bit
   word covering codes 256-319, which is why the word reads c3000000000000. */
static const char XBOX_REAL[] =
    "I: Bus=0005 Vendor=045e Product=02e0 Version=0903\n"
    "N: Name=\"Xbox Wireless Controller\"\n"
    "H: Handlers=kbd event3 \n"
    "B: PROP=0\n"
    "B: EV=1b\n"
    "B: KEY=c3000000000000 0 0 0 0\n"
    "B: ABS=30003\n";

/* A Bluetooth MOUSE: Bus=0005, and it advertises BOTH EV_KEY and EV_ABS
   (many BT mice report ABS for a scroll wheel) -- so the bus check and the
   EV mask alone do NOT reject it. Its KEY bits are in the BTN_MOUSE range
   (0x110/0x111 = 272/273 = local bits 16/17 of the SAME 64-bit word that
   holds BTN_GAMEPAD), not BTN_GAMEPAD's. Only the range check tells the two
   apart, and this fixture is built so the top-16-bits mask genuinely has to
   fire: word[0] is nonzero (0x30000), just not in the bits this project
   reads. */
static const char BT_MOUSE[] =
    "I: Bus=0005 Vendor=046d Product=b02a Version=0011\n"
    "N: Name=\"MX Master Bluetooth Mouse\"\n"
    "H: Handlers=kbd mouse1 event5 \n"
    "B: PROP=0\n"
    "B: EV=1b\n"
    "B: KEY=30000 0 0 0 0\n"
    "B: ABS=100\n";

/* A WIRED (USB) gamepad with the identical KEY/EV shape as the real Bluetooth
   pad above -- everything matches except Bus. Only the bus check tells them
   apart; a USB pad plugged into a Kobo (unlikely hardware, but the record
   format does not lie about it) must not be adopted as a gamepad by a filter
   that is supposed to be finding a BLUETOOTH device. */
static const char USB_PAD[] =
    "I: Bus=0003 Vendor=054c Product=0ce6 Version=0100\n"
    "N: Name=\"Wireless Controller\"\n"
    "H: Handlers=event6 js1 \n"
    "B: PROP=0\n"
    "B: EV=1b\n"
    "B: KEY=c3000000000000 0 0 0 0\n"
    "B: ABS=30003\n";

/* Bus=0005 and BTN_GAMEPAD-range keys present, but EV does not advertise
   EV_ABS (bit 3). Synthetic -- no real device is this self-contradictory --
   but it isolates the EV_ABS check: without it, this passes on Bus and KEY
   alone. */
static const char PAD_NO_ABS[] =
    "I: Bus=0005 Vendor=057e Product=2009 Version=0001\n"
    "N: Name=\"Pro Controller\"\n"
    "H: Handlers=event8 \n"
    "B: PROP=0\n"
    "B: EV=3\n"
    "B: KEY=c3000000000000 0 0 0 0\n";

/* Same idea, isolating EV_KEY (bit 1) instead: Bus=0005, EV_ABS present,
   BTN_GAMEPAD-range bits present in KEY=, but EV does not advertise EV_KEY. */
static const char PAD_NO_KEY_BIT[] =
    "I: Bus=0005 Vendor=057e Product=2009 Version=0001\n"
    "N: Name=\"Pro Controller\"\n"
    "H: Handlers=event9 \n"
    "B: PROP=0\n"
    "B: EV=9\n"
    "B: KEY=c3000000000000 0 0 0 0\n";

TEST_MAIN({
    CHECK_EQ_INT(btinput_is_gamepad(PAD), 1);
    CHECK_EQ_INT(btinput_is_gamepad(XBOX_REAL), 1);

    /* None of the device's own nodes may be mistaken for a controller. If any
       of these were adopted koboy would read the touchscreen twice, or feed
       the accelerometer into the joypad. */
    CHECK_EQ_INT(btinput_is_gamepad(GPIO_KEYS), 0);
    CHECK_EQ_INT(btinput_is_gamepad(TOUCH), 0);
    CHECK_EQ_INT(btinput_is_gamepad(ACCEL), 0);
    CHECK_EQ_INT(btinput_is_gamepad(BT_KBD), 0);

    /* Each of the four AND-ed conditions rejects something that passes the
       OTHER three -- so no single condition is doing the work of all of
       them, and none is redundant with another. */
    CHECK_EQ_INT(btinput_is_gamepad(BT_MOUSE), 0);       /* range check */
    CHECK_EQ_INT(btinput_is_gamepad(USB_PAD), 0);         /* bus check */
    CHECK_EQ_INT(btinput_is_gamepad(PAD_NO_ABS), 0);      /* EV_ABS check */
    CHECK_EQ_INT(btinput_is_gamepad(PAD_NO_KEY_BIT), 0);  /* EV_KEY check */

    /* Handler extraction picks eventN and ignores js0, kbd, mouse0. */
    char node[64];
    CHECK_EQ_INT(btinput_parse_handlers(PAD, node, sizeof node), 1);
    CHECK(strcmp(node, "event3") == 0);
    CHECK_EQ_INT(btinput_parse_handlers(TOUCH, node, sizeof node), 1);
    CHECK(strcmp(node, "event1") == 0);
    CHECK_EQ_INT(btinput_parse_handlers(XBOX_REAL, node, sizeof node), 1);
    CHECK(strcmp(node, "event3") == 0);

    /* A record with no event handler is reported, not guessed at. */
    CHECK_EQ_INT(btinput_parse_handlers(
        "N: Name=\"x\"\nH: Handlers=js0 \n", node, sizeof node), 0);

    /* Malformed input must not overrun. */
    char tiny[4];
    CHECK_EQ_INT(btinput_parse_handlers(PAD, tiny, sizeof tiny), 0);
    CHECK_EQ_INT(btinput_is_gamepad(""), 0);
    CHECK_EQ_INT(btinput_is_gamepad("H: Handlers="), 0);
    CHECK_EQ_INT(btinput_is_gamepad(NULL), 0);
    CHECK_EQ_INT(btinput_parse_handlers(NULL, node, sizeof node), 0);
    CHECK_EQ_INT(btinput_parse_handlers(PAD, NULL, sizeof node), 0);
    CHECK_EQ_INT(btinput_parse_handlers(PAD, node, 0), 0);

    /* A record whose KEY= line is too short to reach the BTN_GAMEPAD word at
       all (rather than reaching it and finding it zero) is also not a
       gamepad -- the "not enough data" path, distinct from the "data says
       no" path BT_MOUSE exercises above. */
    static const char SHORT_KEY[] =
        "I: Bus=0005 Vendor=0000 Product=0000 Version=0000\n"
        "N: Name=\"short\"\n"
        "H: Handlers=event9 \n"
        "B: EV=1b\n"
        "B: KEY=6\n";
    CHECK_EQ_INT(btinput_is_gamepad(SHORT_KEY), 0);

    /* btinput_scan reads the real /proc/bus/input/devices on this host --
       there is no gamepad attached to a CI/dev box, so it must not crash and
       must not fabricate a node. A too-small buffer must not overrun either. */
    char scanbuf[128];
    int r = btinput_scan(scanbuf, sizeof scanbuf);
    CHECK(r == -1 || r == 0 || r == 1);
    char tiny_scan[1];
    int r2 = btinput_scan(tiny_scan, sizeof tiny_scan);
    CHECK(r2 == -1 || r2 == 0 || r2 == 1);
    CHECK_EQ_INT(btinput_scan(NULL, 128), -1);
    CHECK_EQ_INT(btinput_scan(scanbuf, 0), -1);
})
