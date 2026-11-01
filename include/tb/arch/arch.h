/* ==========================================================================
 *  tb/arch/arch.h — архитектурные хот-споты: x86-64 asm + переносимый C.
 *
 *  Принцип «хот-спот + fallback»: каждая оптимизация имеет чистый
 *  C-вариант с идентичной семантикой; asm-путь выбирается диспетчером
 *  в рантайме (cpuid). Это позволяет собирать ядро и на других
 *  архитектурах без изменения кода.
 *
 *    tb_cpu_pause  — PAUSE в спин-ожидании (SMT-friendly)
 *    tb_rdtsc      — счётчик тактов (сид ГПСЧ, микробенчмарки)
 *    tb_crc32c     — CRC-32C (Castagnoli): SSE4.2 crc32q/crc32b | таблица
 *    tb_spinlock   — спинлок на C11 atomics + PAUSE
 *    tb_hash64     — 64-битный хеш для дедупликации (FNV-1a + crc32c-микс)
 * ========================================================================== */
#ifndef TB_ARCH_ARCH_H
#define TB_ARCH_ARCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Инициализация (кэширование cpuid-флагов); вызывается лениво автоматически. */
void tb_arch_init(void);

/* Строка описания: "x86-64, crc32c=sse4.2, rdtsc=asm, pause=asm" или портативный вариант. */
const char *tb_arch_desc(char *buf, size_t cap);

void     tb_cpu_pause(void);
uint64_t tb_rdtsc(void);

bool     tb_cpu_has_sse42(void);
uint32_t tb_crc32c(uint32_t crc, const void *data, size_t len);   /* диспетчер */
uint32_t tb_crc32c_hw(uint32_t crc, const void *data, size_t len); /* SSE4.2 */
uint32_t tb_crc32c_sw(uint32_t crc, const void *data, size_t len); /* таблица */

/* ---- Спинлок ---- */
typedef struct {
    int v; /* 0 — свободен, 1 — занят; доступ только через функции ниже */
} tb_spinlock_t;
#define TB_SPINLOCK_INIT { 0 }

void tb_spin_lock(tb_spinlock_t *l);
bool tb_spin_trylock(tb_spinlock_t *l);
void tb_spin_unlock(tb_spinlock_t *l);

/* ---- Обёртки над GCC-атомиками: совместимы с C и C++ (без <stdatomic.h>) ---- */
bool tb_atomic_load_bool(const volatile bool *p);
void tb_atomic_store_bool(volatile bool *p, bool v);
/* RELAXED-счётчики: инкремент из многих потоков, чтение для статистики. */
uint64_t tb_atomic_add_u64(volatile uint64_t *p, uint64_t v);
uint64_t tb_atomic_load_u64(const volatile uint64_t *p);

/* ---- Хеш для дедупликации ---- */
uint64_t tb_hash64(const void *data, size_t len, uint64_t seed);

#ifdef __cplusplus
}
#endif
#endif /* TB_ARCH_ARCH_H */
