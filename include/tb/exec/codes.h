/* ==========================================================================
 *  tb/exec/codes.h — коды ошибок механизма торговли (домен USER).
 *
 *  Происхождение кодов — перенос семантики контракта-референса
 *  MultiHopSwap (Solidity / Uniswap V3) на стек ядра:
 *
 *    контракт                    здесь
 *    --------------------------  ---------------------------------------
 *    ZeroAmount()                TB_EXEC_ZERO_AMOUNT
 *    DeadlineInPast()            TB_EXEC_DEADLINE_PAST
 *    InvalidPath()               TB_EXEC_INVALID_PATH
 *    WrongMsgValue()             TB_EXEC_VALUE_MISMATCH (инвариант слота)
 *    EthTransferFailed()         TB_EXEC_SETTLE_FAILED
 *    NotWeth() (guard receive)   TB_EXEC_NOT_WRAPPED
 *    amountOutMinimum (revert)   TB_EXEC_SLIPPAGE
 *
 *  Коды ниже ZeroAmount..SETTLE_FAILED — «перенесённые»; дальше — наши
 *  собственные причины (кошелёк, риск-политики, бэкенд, PENDING-таймаут).
 *  Канонические метаданные (имя/severity/retryable) — в реестре кодов.
 * ========================================================================== */
#ifndef TB_EXEC_CODES_H
#define TB_EXEC_CODES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TB_EXEC_OK                    = 0,
    /* -- перенесено из контракта-референса -- */
    TB_EXEC_ZERO_AMOUNT           = 1,  /* входная сумма равна нулю */
    TB_EXEC_DEADLINE_PAST         = 2,  /* дедлайн истёк или слишком близко */
    TB_EXEC_INVALID_PATH          = 3,  /* маршрут не является путём V3 */
    TB_EXEC_VALUE_MISMATCH        = 4,  /* дебет/кредит слота не сходится */
    TB_EXEC_SETTLE_FAILED         = 5,  /* фиксация расчёта не удалась */
    TB_EXEC_NOT_WRAPPED           = 6,  /* нативный актив минуя unwrap (инвариант receive()) */
    TB_EXEC_SLIPPAGE              = 7,  /* выход меньше amountOutMinimum */
    /* -- собственные причины механизма -- */
    TB_EXEC_ROUTE_MISMATCH        = 8,  /* маршрут не согласован с ордером */
    TB_EXEC_INSUFFICIENT_BALANCE  = 9,  /* не хватает виртуального баланса */
    TB_EXEC_INSUFFICIENT_LIQUIDITY= 10, /* бэкенд: ликвидности маршрута мало */
    TB_EXEC_ALLOWLIST             = 11, /* токен вне разрешённого списка */
    TB_EXEC_NOTIONAL_LIMIT        = 12, /* сумма ордера выше лимита */
    TB_EXEC_HOPS_LIMIT            = 13, /* слишком много хопов в маршруте */
    TB_EXEC_OVERFLOW              = 14, /* переполнение 256-битной суммы */
    TB_EXEC_TIMEOUT               = 15, /* PENDING не заполнился за лимит опросов */
    TB_EXEC_BACKEND               = 16, /* ошибка бэкенда исполнения (DEX) */
    TB_EXEC_CAPACITY              = 17, /* таблица ордеров/кошелька переполнена */
    TB_EXEC_SEARCH_INCOMPLETE     = 19, /* бюджет проб/RPC исчерпан: поиск
                                         * неполон — НЕ вердикт «невыгодно»,
                                         * окно могло быть пропущено.
                                         * 18 занят BAD_DECIMALS — дубль
                                         * из ревью пойман и исправлен */
    TB_EXEC_BAD_DECIMALS          = 18  /* неверная десятичная строка суммы */
} tb_exec_code_t;

#ifdef __cplusplus
}
#endif
#endif /* TB_EXEC_CODES_H */
