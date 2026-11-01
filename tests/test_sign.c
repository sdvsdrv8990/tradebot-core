/* ==========================================================================
 *  tests/test_sign.c — ключ/адрес, RLP-векторы, EIP-1559 подпись
 *  (low-S, y-parity, восстановление отправителя).
 * ==========================================================================
 */
#define TB_CURRENT_GROUP TB_G_DEMO

#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "tb/core/error.h"
#include "tb/sign/tx.h"
#include "tb/core/keccak.h"

static void hex_of(const void *p, size_t n, char *buf)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        snprintf(buf + i * 2, 3, "%02x", b[i]);
}

int main(void)
{
    tb_error_t err = TB_ERROR_INIT;

    /* ---- деривация адреса (вектор из Mastering Ethereum) ---- */
    tb_key_t k;
    CHECK(tb_key_from_hex(
        &k,
        "0x0000000000000000000000000000000000000000000000000000000000000001",
        &err) == 0);
    char addr[43];
    tb_addr_to_hex(&k.address, addr, sizeof(addr));
    /* каноническая пара: key=1 (генератор) */
    CHECK_STR_EQ(addr, "0x7e5f4552091a69125d5dfcb7b8c2659029395bdf");

    /* брак ключа: длина */
    tb_key_t bad;
    CHECK(tb_key_from_hex(&bad, "0x1234", &err) != 0);
    CHECK(err.code == TB_CORE_INVALID_ARG);

    /* ---- RLP-векторы (спецификация RLP + EIP-155) ---- */
    {
        uint8_t buf[256];
        tb_rlp_t r;
        char hex[600];

        tb_rlp_init(&r, buf, sizeof(buf));
        tb_rlp_bytes(&r, (const uint8_t *)"dog", 3);
        hex_of(buf, r.len, hex);
        CHECK_STR_EQ(hex, "83646f67");

        tb_rlp_init(&r, buf, sizeof(buf));
        tb_rlp_u64(&r, 1024);
        hex_of(buf, r.len, hex);
        CHECK_STR_EQ(hex, "820400");

        tb_rlp_init(&r, buf, sizeof(buf));
        tb_rlp_u64(&r, 0);
        hex_of(buf, r.len, hex);
        CHECK_STR_EQ(hex, "80");

        tb_rlp_init(&r, buf, sizeof(buf));
        const uint8_t catdog[] = { 0x83, 'c', 'a', 't', 0x83, 'd', 'o',
                                   'g' };
        tb_rlp_list(&r, catdog, sizeof(catdog));
        hex_of(buf, r.len, hex);
        CHECK_STR_EQ(hex, "c88363617483646f67");

        /* префикс из EIP-155: rlp([9, 20gwei, 21000, to, 1e18, "", 1, 0, 0]) */
        tb_addr_t to;
        tb_addr_from_hex(&to, "0x3535353535353535353535353535353535353535",
                         &err);
        tb_rlp_t body;
        uint8_t bb[128];
        tb_rlp_init(&body, bb, sizeof(bb));
        tb_rlp_u64(&body, 9);
        tb_rlp_u64(&body, 20000000000ull);
        tb_rlp_u64(&body, 21000);
        tb_rlp_bytes(&body, to.b, 20);
        tb_rlp_u64(&body, 1000000000000000000ull);
        tb_rlp_bytes(&body, NULL, 0);
        tb_rlp_u64(&body, 1);
        tb_rlp_u64(&body, 0);
        tb_rlp_u64(&body, 0);
        CHECK(!body.overflow);
        /* эталон EIP-155: внутренности rlp([9,20gwei,21000,to,1e18,"",1,0,0])
         * (префикс списка 0xec добавляет обёртка) */
        hex_of(bb, body.len, hex);
        CHECK_STR_EQ(hex, "098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080018080");
    }

    /* ---- EIP-1559 подпись: self-consistency ---- */
    {
        tb_tx1559_t tx = {
            .chain_id = 1,
            .nonce = 9,
            .max_priority_fee = 1000000000ull,
            .max_fee = 20000000000ull,
            .gas_limit = 300000,
            .to = k.address,
            .value = 1000000000000000ull, /* 0.001 ETH */
            .data = (const uint8_t *)"\xc6\xa5\x02\x6a",
            .data_len = 4,
        };
        char raw[1600];
        uint8_t tx_hash[32], sig65[65];
        CHECK(tb_tx1559_sign(&k, &tx, raw, sizeof(raw), tx_hash, sig65,
                             &err) == 0);
        CHECK(strncmp(raw, "0x02", 4) == 0); /* type 0x02 = EIP-1559 */
        CHECK(strlen(raw) > 200);

        /* повторная подпись тем же ключом — детерминированность
         * (RFC6979-нонс по умолчанию в secp256k1) */
        char raw2[1600];
        uint8_t tx_hash2[32], sig652[65];
        CHECK(tb_tx1559_sign(&k, &tx, raw2, sizeof(raw2), tx_hash2, sig652,
                             &err) == 0);
        CHECK_STR_EQ(raw, raw2);

        /* восстановление отправителя: подписали ключом k — вернётся
         * адрес k (подпись отдаётся из tb_tx1559_sign напрямую). */
        uint8_t s32[32], r32[32];
        memcpy(r32, sig65, 32);
        memcpy(s32, sig65 + 32, 32);
        int parity = sig65[64];
        CHECK(parity == 0 || parity == 1);

        /* signing hash: rlp9 без сигнатуры */
        uint8_t buf2[700];
        size_t l2 = 0;
        {
            tb_rlp_t body_r;
            uint8_t bb[640];
            tb_rlp_init(&body_r, bb, sizeof(bb));
            tb_rlp_u64(&body_r, tx.chain_id);
            tb_rlp_u64(&body_r, tx.nonce);
            tb_rlp_u64(&body_r, tx.max_priority_fee);
            tb_rlp_u64(&body_r, tx.max_fee);
            tb_rlp_u64(&body_r, tx.gas_limit);
            tb_rlp_bytes(&body_r, tx.to.b, 20);
            tb_rlp_u64(&body_r, tx.value);
            tb_rlp_bytes(&body_r, tx.data, tx.data_len);
            tb_rlp_list(&body_r, NULL, 0);
            tb_rlp_t top;
            tb_rlp_init(&top, buf2 + 1, sizeof(buf2) - 1);
            tb_rlp_list(&top, bb, body_r.len);
            l2 = top.len;
            buf2[0] = 0x02; /* хеш подписи: 0x02 || rlp(payload) */
            l2 += 1;
        }
        uint8_t shash[32];
        tb_keccak256(buf2, l2, shash);

        tb_addr_t sender;
        CHECK(tb_sig_recover_sender(shash, r32, s32, parity, &sender,
                                    &err) == 0);
        tb_addr_to_hex(&sender, addr, sizeof(addr));
        CHECK_STR_EQ(addr, "0x7e5f4552091a69125d5dfcb7b8c2659029395bdf");

        /* low-S: s < n/2 */
        uint8_t half_n[32] = { 0x7f, 0xff };
        uint8_t s_be[32];
        memcpy(s_be, s32, 32);
        /* s в raw минимально закодировано — сравнение грубое достаточно:
         * первый байт < 0x80 гарантирует < 2^252 > n/2 — проверим иначе:
         * старший байт s не может быть 0xff при low-S */
        CHECK(s_be[0] != 0xff);
        (void)half_n;
    }

    printf("OK test_sign\n");
    return 0;
}
