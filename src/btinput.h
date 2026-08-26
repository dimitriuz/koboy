#ifndef KOBOY_BTINPUT_H
#define KOBOY_BTINPUT_H
#include <stdbool.h>
#include <stddef.h>

/* Finding a Bluetooth gamepad, which to koboy is just another key device.

   bluetoothd's `input` plugin turns a paired HID device into an ordinary
   /dev/input/eventN node, and input.c already decodes those -- so the whole
   of "controller support" is finding the right node and reading it. No
   libbluetooth, no D-Bus, no new dependency: this parses
   /proc/bus/input/devices.

   The parsing is split out and pure so the filter is tested against the real
   records of the verified device (spec Appendix A, and a real Xbox Wireless
   Controller record measured 2026-08-26) rather than against a device that
   must be physically present to run the tests. */

/* True for one /proc/bus/input/devices record that looks like a gamepad:
   BUS_BLUETOOTH (Bus=0005), both EV_KEY and EV_ABS advertised, and at least
   one key in the BTN_GAMEPAD range (0x130-0x13F). All three are required --
   see btinput.c for which real device each one alone would otherwise let
   through. */
bool btinput_is_gamepad(const char *record);

/* Extracts the eventN handler from a record's `H: Handlers=` line into
   `out`. Returns 1 on success, 0 if there is no event handler, `record` is
   malformed, or `out` is too small to hold it (never truncates). */
int  btinput_parse_handlers(const char *record, char *out, size_t n);

/* Scans /proc/bus/input/devices for the first gamepad record and writes its
   node path ("/dev/input/eventN") into out_node. 1 = found, 0 = none,
   -1 = cannot read the file. */
int  btinput_scan(char *out_node, size_t n);
#endif
