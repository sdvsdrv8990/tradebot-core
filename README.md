# tradebot-core — ядро торгового бота

Ядро децентрализованной арбитражной торговли на C11/C++20 (Linux):
событийный планировщик, реестр событий с цепочками шагов, Event Ledger,
конвейер исполнения с атомарными слотами, живые котировки Uniswap V3
(QuoterV2 через публичный RPC) и живое исполнение из Safe
(EIP-1559, EIP-712). Фильтры 1–4 + Decision Engine ищут минимальную
выгодную ставку межпулового арбитража WETH→TOKEN→WETH.

Подробности устройства — в [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Развёртывание и запуск

Одна команда из корневого каталога: при необходимости собирает
(зависимости локально в `.deps/`, без root) и поднимает бота вместе с
веб-мордой. Привязки ко времени нет — работает до остановки.

Установка

```bash
./run.sh            # бот + веб-морда http://127.0.0.1:8080 (режим — из конфига)
./run.sh stop       # остановка (то же: Ctrl+C, kill <pid>) — итоги, журнал
./run.sh addr       # адрес ключа бота — для добавления владельцем в Safe
```

Запуск

```bash
./run.sh addr     # адрес ключа бота — его добавляешь владельцем в Safe
./run.sh          # бой: бот + веб-морда http://127.0.0.1:8080
./run.sh stop     # остановка (Ctrl+C / kill тоже работают)
```

### Боевой запуск (live-exec)

В бою всё уже реальные данные: котировки — `eth_call` в Uniswap
QuoterV2, сделки — настоящие транзакции из Safe, выход — по receipt и
дельте `balanceOf`, PnL — по балансу контракта. Отдельный парсер не
нужен: не хватает только ключа, адреса контракта и галочки в конфиге.
Тестовые данные в бой не попадают — журнал начинается заново при
каждом запуске.

1. Ключ бота: `openssl rand -hex 32 > bot_key.hex`, затем
   `./run.sh addr` — адрес ключа (0x…).
2. Кошелёк — любой веб- (MetaMask, Rabby, …). Создать Safe на
   [app.safe.global](https://app.safe.global) (Ethereum): первый
   владелец — твой кошелёк, второй — адрес бота, порог 1. Это и есть
   «контракт на N эфира»: бот торгует через `execTransaction` изнутри
   Safe, деньги контракт не покидают; закрытие — снять бота с
   владельцев или вывести всё. Ключ бота горячий: его компрометация
   теряет только содержимое Safe.
3. Пополнить: **WETH на Safe** (торговый капитал) и **немного ETH на
   адрес ключа бота** (~0.01 — газ, ~$0.5–2 за сделку).
4. `arb_config.json` (скопировать из примера): `network.safe` = адрес
   Safe, `network.key_file` = `"bot_key.hex"`, `network.live_exec` =
   `true`. Пределы риска там же: `max_trade_wei` (верх контракта),
   `min_profit_wei`, `max_fee_wei`.
5. `./run.sh` — бой. Сделки видны в app.safe.global и Zerion/DeBank,
   живая статистика — на веб-морде.

Демо подсистем (после сборки лежат в `.build/`):

| Демо | Что показывает |
|---|---|
| `arb_demo` | арбитражный конвейер: фильтры 1–4, Decision Engine (минимальная ставка до верха контракта) |
| `bot_demo` | опрос источников: 429/Retry-After, backoff+jitter, circuit breaker, Event Ledger |
| `events_demo` | реестр событий: цепочка шагов, загрузка/выгрузка на ходу, мосты к планировщику и журналу |
| `exec_demo` | механизм торговли: preflight→commit, атомарность слота, PENDING, slippage-защита |

## Структура проекта

```
tradebot-core/
├── run.sh                     # развёртывание: бот + веб-морда одной командой
├── setup.sh                   # стадии сборки: deps | build | test | всё
├── CMakeLists.txt / Makefile   # сборка (основная + запасной путь без CMake)
├── include/tb/                 # публичные заголовки (C-ABI)
│   ├── core/     #   error, registry, ids, detail, keccak, log, time, alloc
│   ├── sched/    #   task, pqueue, timer, loop, threadpool, backoff
│   ├── runtime/  #   ratelimit, breaker
│   ├── events/   #   events (реестр), builtins (шаги-шлюзы)
│   ├── ledger/   #   ledger (JSONL-журнал)
│   ├── exec/     #   codes, u256, route, wallet, backend, backend_dex,
│   │             #   safe_exec (живое исполнение из Safe), engine
│   ├── net/      #   http (транспорт net + TLS / replay)
│   ├── sign/     #   tx (ключ, RLP, EIP-1559)
│   ├── user/     #   arb (фильтры 1–4, Decision Engine, рантайм)
│   ├── json/     #   envelope, config (C++/rapidjson с C-ABI)
│   └── arch/     #   arch (asm-хот-споты)
├── src/            # реализация (зеркалит include/)
├── tests/          # 26 тестов
├── docs/           # ARCHITECTURE.md, образцы журналов, arb_config.example.json
└── third_party/    # rapidjson, mbedtls, secp256k1 (вендор)
```

Библиотеки: `tb_core` (C11), `tb_net` (HTTP/TLS), `tb_sign`
(EIP-1559), `tb_dex` (DEX-адаптер + Safe), `tb_user` (арбитраж),
`tb_runtime` (C++/rapidjson). Демо и тесты линкуются против них.
