/* ==========================================================================
 *  tb/core/keccak.h — keccak-256 (оригинальный паддинг Keccak, как в EVM).
 *
 *  Не NIST SHA3 (другой паддинг)! Нужен для: селекторов функций ABI,
 *  адресов из публичных ключей, хешей транзакций (EIP-1559) и
 *  типизированных данных EIP-712 (SafeTx).
 * ==========================================================================
 */
#ifndef TB_CORE_KECCAK_H
#define TB_CORE_KECCAK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_KECCAK256_LEN 32

/* keccak256(data, len) -> out32. */
void tb_keccak256(const void *data, size_t len, uint8_t out[32]);

/* Селектор функции: первые 4 байта keccak256(sig). */
void tb_keccak_selector(const char *signature, uint8_t out[4]);

#ifdef __cplusplus
}
#endif
#endif /* TB_CORE_KECCAK_H */
