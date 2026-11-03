#define TB_CURRENT_GROUP TB_G_CORE /* зона ответственности файла */

/* ==========================================================================
 *  tb/core/keccak.c — keccak-f[1600] с оригинальным паддингом (0x01).
 *
 *  Эталон: селекторы Uniswap V3 QuoterV2 (0xc6a5026a), адреса из ключей.
 * ========================================================================== */
#include "tb/core/keccak.h"

#include <string.h>

/* Раундовые константы */
static const uint64_t RC[24] = {
    0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull,
    0x8000000080008000ull, 0x000000000000808bull, 0x0000000080000001ull,
    0x8000000080008081ull, 0x8000000000008009ull, 0x000000000000008aull,
    0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull,
    0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull,
    0x8000000000008003ull, 0x8000000000008002ull, 0x8000000000000080ull,
    0x000000000000800aull, 0x800000008000000aull, 0x8000000080008081ull,
    0x8000000000008080ull, 0x0000000080000001ull, 0x0000000080008008ull,
};

static const int RHO[5][5] = {
    { 0, 36, 3, 41, 18 },
    { 1, 44, 10, 45, 2 },
    { 62, 6, 43, 15, 61 },
    { 28, 55, 25, 21, 56 },
    { 27, 20, 39, 8, 14 },
};

static inline uint64_t rotl64(uint64_t x, int n)
{
    /* n == 0: сдвиг на 64 (ширину типа) — UB; gcc «прощал», clang
     * оптимизировал по предположению и портил хеш (CI Linux/clang:
     * падали test_keccak/test_sign/test_backend_dex — селекторы) */
    n &= 63;
    return n == 0 ? x : ((x << n) | (x >> (64 - n)));
}

static void keccakf(uint64_t A[25])
{
    for (int rnd = 0; rnd < 24; rnd++) {
        uint64_t C[5], D[5], B[25];

        for (int x = 0; x < 5; x++)
            C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
        for (int x = 0; x < 5; x++)
            D[x] = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                A[x + 5 * y] ^= D[x];

        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                B[y + 5 * ((2 * x + 3 * y) % 5)] =
                    rotl64(A[x + 5 * y], RHO[x][y]);

        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                A[x + 5 * y] =
                    B[x + 5 * y]
                    ^ ((~B[(x + 1) % 5 + 5 * y]) & B[(x + 2) % 5 + 5 * y]);

        A[0] ^= RC[rnd];
    }
}

void tb_keccak256(const void *data, size_t len, uint8_t out[32])
{
    uint64_t A[25] = { 0 };
    uint8_t block[136]; /* rate = 1088 бит */
    const uint8_t *p = data;
    size_t rem = len;

    /* Последний блок: 0x01, нули, старший байт |= 0x80 */
    while (rem >= 136) {
        for (int i = 0; i < 136 / 8; i++) {
            uint64_t w;
            memcpy(&w, (const uint8_t *)p + i * 8, 8);
            A[i % 5 + 5 * (i / 5)] ^= w; /* little-endian, iota-раскладка */
        }
        keccakf(A);
        p += 136;
        rem -= 136;
    }

    memset(block, 0, sizeof(block));
    if (rem)
        memcpy(block, p, rem);
    block[rem] = 0x01; /* Keccak (не SHA3: там 0x06) */
    block[135] |= 0x80;

    for (int i = 0; i < 136 / 8; i++) {
        uint64_t w;
        memcpy(&w, block + i * 8, 8);
        A[i % 5 + 5 * (i / 5)] ^= w;
    }
    keccakf(A);

    for (int i = 0; i < 4; i++)
        memcpy(out + i * 8, &A[i], 8); /* первые 256 бит, little-endian */
}

void tb_keccak_selector(const char *signature, uint8_t out[4])
{
    uint8_t h[32];
    tb_keccak256(signature, strlen(signature), h);
    memcpy(out, h, 4);
}
