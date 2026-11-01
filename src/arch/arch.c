#define TB_CURRENT_GROUP TB_G_ARCH /* зона ответственности файла */
/* ==========================================================================
 *  tb/arch/arch.c — хот-споты: x86-64 asm + переносимый C-fallback.
 *
 *  Правило проекта: ассемблер — только в точках, где он даёт измеримую
 *  пользу, и всегда рядом существует чистый C-вариант с идентичной
 *  семантикой. Выбор пути — в рантайме (cpuid), поэтому ядро собирается
 *  и на платформах без этих инструкций.
 *
 *  Здесь:
 *    PAUSE   — корректное поведение при SMT в спин-циклах;
 *    RDTSC   — дешёвый источник энтропии для сидирования ГПСЧ;
 *    CRC32C  — аппаратная инструкция SSE4.2 (crc32q/crc32b) против
 *              табличной реализации полинома Castagnoli 0x1EDC6F41;
 *    Spinlock — C11-подобные атомики через GCC builtins (работают и в C++).
 * ========================================================================== */
#include "tb/arch/arch.h"
#include "tb/core/time.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#define TB_X86 1
#endif

/* ---- Ленивая инициализация ---- */
static volatile bool g_arch_ready;
static bool g_has_sse42;
static uint32_t g_crc_table[256];

static void crc32c_build_table(void)
{
    /* Отражённый полином CRC-32C (Castagnoli): 0x82F63B78. */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0x82F63B78u & (uint32_t)(0u - (c & 1u)));
        g_crc_table[i] = c;
    }
}

static void arch_init_once(void)
{
#if TB_X86
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1u), "c"(0u));
    (void)eax; (void)ebx; (void)edx;
    g_has_sse42 = (ecx >> 20) & 1u; /* CPUID.1:ECX.SSE4.2 */
#endif
    crc32c_build_table();
    __atomic_store_n(&g_arch_ready, true, __ATOMIC_RELEASE);
}

void tb_arch_init(void)
{
    if (!__atomic_load_n(&g_arch_ready, __ATOMIC_ACQUIRE))
        arch_init_once();
}

/* ---- Пауза спина ---- */
void tb_cpu_pause(void)
{
#if TB_X86
    __asm__ __volatile__("pause");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* ---- Счётчик тактов ---- */
uint64_t tb_rdtsc(void)
{
#if TB_X86
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return tb_now_us();
#endif
}

bool tb_cpu_has_sse42(void)
{
    tb_arch_init();
    return g_has_sse42;
}

/* ---- CRC-32C: аппаратный путь (SSE4.2) ---- */
uint32_t tb_crc32c_hw(uint32_t crc, const void *data, size_t len)
{
#if TB_X86
    const uint8_t *p = (const uint8_t *)data;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);          /* избегаем требований к выравниванию */
        uint64_t c = crc;
        __asm__ __volatile__("crc32q %1, %0" : "+r"(c) : "rm"(v));
        crc = (uint32_t)c;
        p += 8;
        len -= 8;
    }
    while (len--) {
        __asm__ __volatile__("crc32b %1, %0" : "+r"(crc) : "rm"(*p));
        p++;
    }
    return crc;
#else
    return tb_crc32c_sw(crc, data, len);
#endif
}

/* ---- CRC-32C: табличный fallback ---- */
uint32_t tb_crc32c_sw(uint32_t crc, const void *data, size_t len)
{
    tb_arch_init(); /* гарантирует таблицу */
    const uint8_t *p = (const uint8_t *)data;
    while (len--)
        crc = (crc >> 8) ^ g_crc_table[(crc ^ *p++) & 0xFFu];
    return crc;
}

/* ---- Диспетчер ---- */
uint32_t tb_crc32c(uint32_t crc, const void *data, size_t len)
{
    tb_arch_init();
    return g_has_sse42 ? tb_crc32c_hw(crc, data, len)
                       : tb_crc32c_sw(crc, data, len);
}

const char *tb_arch_desc(char *buf, size_t cap)
{
    tb_arch_init();
#if TB_X86
    snprintf(buf, cap, "x86-64 (System V ABI), pause=asm, rdtsc=asm, crc32c=%s",
             g_has_sse42 ? "sse4.2-asm" : "sw-table");
#else
    snprintf(buf, cap, "portable C (asm-пути отключены)");
#endif
    return buf;
}

/* ---- Спинлок ---- */
void tb_spin_lock(tb_spinlock_t *l)
{
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&l->v, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        /* Билетная фаза ожидания: не грузим шину, отдаём конвейер. */
        while (__atomic_load_n(&l->v, __ATOMIC_RELAXED))
            tb_cpu_pause();
    }
}

bool tb_spin_trylock(tb_spinlock_t *l)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&l->v, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void tb_spin_unlock(tb_spinlock_t *l)
{
    __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE);
}

/* ---- 64-битный хеш для дедупликации ---- */
static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t tb_hash64(const void *data, size_t len, uint64_t seed)
{
    /* FNV-1a 64 */
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xcbf29ce484222325ull ^ seed;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3full;
    }
    /* Подмешивание CRC-32C: на x86-64 считается одной инструкцией на байт. */
    uint32_t c = tb_crc32c(0, data, len);
    return mix64(h ^ ((uint64_t)c << 32) ^ (seed * 0xD6E8FEB86659FD93ull));
}

/* ---- Обёртки атомиков, совместимые с C и C++ ---- */
bool tb_atomic_load_bool(const volatile bool *p)
{
    return __atomic_load_n((const bool *)p, __ATOMIC_SEQ_CST);
}

void tb_atomic_store_bool(volatile bool *p, bool v)
{
    __atomic_store_n((bool *)p, v, __ATOMIC_SEQ_CST);
}
