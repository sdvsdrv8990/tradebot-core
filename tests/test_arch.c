/* tests/test_arch.c — asm-хот-споты против переносимых fallback-ов. */
#include "harness.h"
#include "tb/arch/arch.h"

int main(void)
{
    tb_arch_init();

    /* ---- CRC-32C: эталонное значение ("123456789", init 0xFFFFFFFF,
     * финальный XOR) = 0xE3069283 — полином Castagnoli. ---- */
    uint32_t raw = tb_crc32c_sw(0xFFFFFFFFu, "123456789", 9);
    CHECK(raw == (0xE3069283u ^ 0xFFFFFFFFu));
    CHECK((raw ^ 0xFFFFFFFFu) == 0xE3069283u);
    CHECK(tb_crc32c_sw(0, "", 0) == 0);

    /* ---- аппаратный путь совпадает с табличным ---- */
    char buf[4096];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (char)(i * 31u + 7u);
    uint32_t sw = tb_crc32c_sw(0, buf, sizeof(buf));
    if (tb_cpu_has_sse42()) {
        uint32_t hw = tb_crc32c_hw(0, buf, sizeof(buf));
        CHECK(hw == sw);
        CHECK(tb_crc32c(0, buf, sizeof(buf)) == sw); /* диспетчер выбрал hw */
    }

    /* ---- rdtsc монотонен ---- */
    uint64_t t1 = tb_rdtsc();
    uint64_t t2 = tb_rdtsc();
    CHECK(t2 >= t1);

    /* ---- спинлок ---- */
    tb_spinlock_t lock = TB_SPINLOCK_INIT;
    CHECK(tb_spin_trylock(&lock) == true);
    CHECK(tb_spin_trylock(&lock) == false); /* уже занят */
    tb_spin_unlock(&lock);
    tb_spin_lock(&lock);
    tb_spin_unlock(&lock);
    CHECK(tb_spin_trylock(&lock) == true);
    tb_spin_unlock(&lock);

    /* ---- 64-битный хеш ---- */
    CHECK(tb_hash64("abc", 3, 1) == tb_hash64("abc", 3, 1));
    CHECK(tb_hash64("abc", 3, 1) != tb_hash64("abd", 3, 1));
    CHECK(tb_hash64("abc", 3, 1) != tb_hash64("abc", 3, 2));

    char desc[128];
    CHECK(tb_arch_desc(desc, sizeof(desc)) != NULL);
    CHECK(desc[0] != '\0');
    printf("OK test_arch (%s)\n", desc);

    /* Наглядность: скорость аппаратного CRC против табличного. */
    if (tb_cpu_has_sse42()) {
        uint64_t s = tb_rdtsc();
        volatile uint32_t acc = 0;
        for (int i = 0; i < 64; i++)
            acc += tb_crc32c_hw(0, buf, sizeof(buf));
        uint64_t hw_cycles = tb_rdtsc() - s;
        s = tb_rdtsc();
        for (int i = 0; i < 64; i++)
            acc += tb_crc32c_sw(0, buf, sizeof(buf));
        uint64_t sw_cycles = tb_rdtsc() - s;
        printf("     crc32c 4 КиБ x64: asm=%llu тактов, таблица=%llu тактов (%0.1fx)\n",
               (unsigned long long)hw_cycles, (unsigned long long)sw_cycles,
               (double)sw_cycles / (double)hw_cycles);
        (void)acc;
    }
    return 0;
}
