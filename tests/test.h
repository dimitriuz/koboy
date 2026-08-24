#ifndef KOBOY_TEST_H
#define KOBOY_TEST_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_run = 0, t_fail = 0;

#define CHECK(cond) do { t_run++; if (!(cond)) { t_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ_INT(a, b) do { t_run++; long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { t_fail++; fprintf(stderr, "FAIL %s:%d: %s == %ld, expected %ld\n", \
        __FILE__, __LINE__, #a, _a, _b); } } while (0)

#define TEST_MAIN(body) int main(void) { body; \
    fprintf(stderr, "%s: %d checks, %d failures\n", __FILE__, t_run, t_fail); \
    return t_fail ? 1 : 0; }
#endif
