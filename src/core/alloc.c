#define TB_CURRENT_GROUP TB_G_CORE /* зона ответственности файла */
/* ==========================================================================
 *  tb/core/alloc.c — arena-аллокатор.
 * ========================================================================== */
#include "tb/core/alloc.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define TB_ARENA_DEFAULT_BLOCK (64u * 1024u)
#define TB_ARENA_MAX_ALIGN     64u

typedef struct tb_arena_block {
    struct tb_arena_block *next;
    size_t size;      /* полезная ёмкость data */
    size_t used;
    /* data[] следует за заголовком с выравниванием */
} tb_arena_block_t;

struct tb_arena {
    tb_arena_block_t *head;     /* список сегментов, первый — активный */
    size_t block_size;
    size_t total_reserved;
};

static uint8_t *block_data(tb_arena_block_t *b)
{
    return (uint8_t *)b + sizeof(tb_arena_block_t);
}

tb_arena_t *tb_arena_create(size_t block_size, tb_error_t *err)
{
    if (block_size == 0) block_size = TB_ARENA_DEFAULT_BLOCK;
    tb_arena_t *a = calloc(1, sizeof(tb_arena_t));
    if (!a) {
        TB_ERROR_SET(err, TB_DOMAIN_CORE, TB_CORE_NO_MEMORY,
                     "arena: нет памяти под дескриптор");
        return NULL;
    }
    a->block_size = block_size;
    return a;
}

static tb_arena_block_t *block_new(size_t need)
{
    size_t sz = sizeof(tb_arena_block_t) + need;
    tb_arena_block_t *b = malloc(sz);
    if (!b) return NULL;
    b->next = NULL;
    b->size = need;
    b->used = 0;
    return b;
}

void *tb_arena_alloc(tb_arena_t *arena, size_t size, size_t align)
{
    if (!arena) return NULL;
    if (align == 0) align = alignof(max_align_t);
    if (align > TB_ARENA_MAX_ALIGN) align = TB_ARENA_MAX_ALIGN;

    /* Крупные выделения получают собственный сегмент. */
    size_t need = (size > arena->block_size) ? size : arena->block_size;

    tb_arena_block_t *b = arena->head;
    if (!b || b->size - b->used < size + align) {
        tb_arena_block_t *nb = block_new(need);
        if (!nb) return NULL;
        nb->next = arena->head;
        arena->head = nb;
        arena->total_reserved += nb->size;
        b = nb;
    }

    uintptr_t base = (uintptr_t)block_data(b) + b->used;
    uintptr_t aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t skip = (size_t)(aligned - base);
    if (b->size - b->used < skip + size) {
        /* Выравнивание не влезло в хвост — новый сегмент. */
        tb_arena_block_t *nb = block_new(need);
        if (!nb) return NULL;
        nb->next = arena->head;
        arena->head = nb;
        arena->total_reserved += nb->size;
        b = nb;
        base = (uintptr_t)block_data(b);
        aligned = (base + (align - 1)) & ~(uintptr_t)(align - 1);
        skip = (size_t)(aligned - base);
    }
    b->used += skip + size;
    return (void *)aligned;
}

char *tb_arena_strdup(tb_arena_t *arena, const char *s)
{
    return tb_arena_strndup(arena, s, s ? strlen(s) : 0);
}

char *tb_arena_strndup(tb_arena_t *arena, const char *s, size_t n)
{
    char *p = tb_arena_alloc(arena, n + 1, 1);
    if (!p) return NULL;
    if (n && s) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void tb_arena_reset(tb_arena_t *arena)
{
    if (!arena) return;
    /* Первый сегмент сохраняем, остальные освобождаем. */
    if (arena->head) {
        tb_arena_block_t *rest = arena->head->next;
        while (rest) {
            tb_arena_block_t *next = rest->next;
            arena->total_reserved -= rest->size;
            free(rest);
            rest = next;
        }
        arena->head->next = NULL;
        arena->head->used = 0;
        arena->total_reserved = arena->head->size;
    }
}

size_t tb_arena_used(const tb_arena_t *arena)
{
    if (!arena) return 0;
    size_t sum = 0;
    for (const tb_arena_block_t *b = arena ? arena->head : NULL; b; b = b->next)
        sum += b->used;
    return sum;
}

size_t tb_arena_reserved(const tb_arena_t *arena)
{
    return arena ? arena->total_reserved : 0;
}

void tb_arena_destroy(tb_arena_t *arena)
{
    if (!arena) return;
    tb_arena_block_t *b = arena->head;
    while (b) {
        tb_arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    free(arena);
}
