/* ==========================================================================
 *  tb/core/alloc.h — arena-аллокатор (bump-allocator).
 *
 *  Назначение: массовые короткоживущие выделения внутри одной итерации
 *  цикла (разбор ответов, формирование событий) без поштучного free().
 *  reset() обнуляет всю арену одним движением.
 * ========================================================================== */
#ifndef TB_CORE_ALLOC_H
#define TB_CORE_ALLOC_H

#include <stddef.h>
#include <stdint.h>
#include "tb/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_arena tb_arena_t;

/* block_size — размер сегментов (0 -> 64 КиБ). */
tb_arena_t *tb_arena_create(size_t block_size, tb_error_t *err);
void        tb_arena_destroy(tb_arena_t *arena);

void  *tb_arena_alloc(tb_arena_t *arena, size_t size, size_t align);
char  *tb_arena_strdup(tb_arena_t *arena, const char *s);
char  *tb_arena_strndup(tb_arena_t *arena, const char *s, size_t n);

/* Все прежние выделения недействительны. */
void tb_arena_reset(tb_arena_t *arena);

size_t tb_arena_used(const tb_arena_t *arena);     /* полезные байты */
size_t tb_arena_reserved(const tb_arena_t *arena); /* выделено сегментов */

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_ALLOC_H */
