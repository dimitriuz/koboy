#ifndef KOBOY_SRAM_H
#define KOBOY_SRAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void sram_path_for_rom(char *out, size_t outlen, const char *save_dir,
                       const char *rom_path);
bool sram_save(const char *path, const uint8_t *src, size_t len);
bool sram_load(const char *path, uint8_t *dst, size_t len);
#endif
