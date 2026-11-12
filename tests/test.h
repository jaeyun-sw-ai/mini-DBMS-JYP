#ifndef TEST_H
#define TEST_H
#include <stdio.h>
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define TEST_END() do { printf("%-28s %5d checks, %d failed\n", __FILE__, g_checks, g_fail); return g_fail ? 1 : 0; } while (0)
#endif
