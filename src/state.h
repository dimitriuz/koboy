#ifndef KOBOY_STATE_H
#define KOBOY_STATE_H
#include <stdbool.h>
#include <stddef.h>

#define KOBOY_STATE_SLOTS 3

/* <save_dir>/<rom stem>.st<N>, derived the same way .srm is, so two games never
   share a slot. Slots are 1-based; an out-of-range slot writes "". */
void state_path(char *out, size_t n, const char *save_dir,
                const char *rom_path, int slot);

bool state_exists(const char *save_dir, const char *rom_path, int slot);

/* A menu row: "SLOT 1 - SAVED" or "SLOT 2 - EMPTY". Knowing which slot you are
   about to overwrite is most of the value of having slots. */
void state_slot_label(char *out, size_t n, const char *save_dir,
                      const char *rom_path, int slot);
#endif
