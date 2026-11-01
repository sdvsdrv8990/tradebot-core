/* tests/harness.h — минимальный каркас юнит-тестов (zero-dep). */
#ifndef TB_TEST_HARNESS_H
#define TB_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do {                                                  \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        exit(1);                                                          \
    }                                                                     \
} while (0)

#define CHECK_STR_EQ(a, b) CHECK(strcmp((a), (b)) == 0)

#endif
